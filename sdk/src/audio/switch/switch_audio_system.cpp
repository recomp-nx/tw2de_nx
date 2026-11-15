/**
 * @file        audio/switch/switch_audio_system.cpp
 * @brief       Audio output for Nintendo Switch through libnx audout
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdio>
#include <string>
#include <vector>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/switch/switch_audio_system.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <switch.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

// Passive counters: the audio thread neither writes reports nor pauses the game.
extern "C" void RexSwitchPerfAdd(unsigned id, u64 value);
extern "C" void RexSwitchPerfMax(unsigned id, u64 value);

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_BOOL(audio_switch_pump, true, "Audio",
                    "Ask the game for one frame every 5.333 ms (187.5 Hz pump, like "
                    "MarathonRecomp-NX) instead of 4 in a row when each audout buffer is consumed, which "
                    "left the game's audio server voice without a packet (robotic audio); "
                    "read when the audio is opened");
// TW2 (2026-11-13): the crackles are audible where no request goes without data (the intro video), so the output
// itself is looked at: FillAndAppend keeps what it appends to audout and writes it out once.
REXCVAR_DEFINE_INT32(audio_switch_record_s, 0, "Audio",
                     "Diagnostic: the first N seconds of the mixed output (s16 stereo, 48 kHz, exactly what goes to "
                     "audout) are kept in memory and then written to audio_out.raw in the log folder; 0 = off")
    .range(0, 600);
extern "C" const char* RexSwitchLogDir(void);
namespace {
// audio_switch_record_s: where in the recording (seconds) the pump asked for a second frame in one tick, or for none.
float g_record_seconds = -1.0f;  // -1: not recording
std::vector<float> g_record_double, g_record_none;
}  // namespace
REXCVAR_DEFINE_INT32(audio_switch_frames_in_queue, 10, "Audio",
                     "With audio_switch_pump: game frames kept queued before audout "
                     "(cushion; 10 = 53 ms, 6 up to build 105). A longer queue lets the game wait without cutting the "
                     "output, at the cost of latency: masseffect_audio_wait_server_ms waits up to 30 ms; read "
                     "when the audio is opened");

namespace rex::audio::nx {

namespace {

// Guest frames per audout buffer, and buffers kept queued in the device.
// 4 frames are 1024 samples (21.3 ms), the block size MarathonRecomp-NX found
// to be safe on the shared cores; three buffers cap the latency at ~64 ms.
constexpr uint32_t kFramesPerBuffer = 4;
constexpr uint32_t kBufferCount = 3;
constexpr uint32_t kBufferSamples = kFramesPerBuffer * SwitchAudioDriver::kChannelSamples;
constexpr uint32_t kBufferBytes = kBufferSamples * 2 * sizeof(s16);
// audout wants page-aligned sample memory.
constexpr uint32_t kBufferAllocBytes = (kBufferBytes + 0xFFF) & ~0xFFFu;

// Above guest threads (0x3B) so the device is never starved, like the audio
// pump of MarathonRecomp-NX (0x2B).
constexpr int kOutputThreadPriority = 0x2B;
constexpr size_t kOutputThreadStack = 0x10000;
// Bounded so the thread notices shutdown even if the device stops releasing.
constexpr u64 kWaitTimeoutNs = 100'000'000;
// audio_switch_pump: one frame (256 samples) every 5.333 ms with absolute deadlines. With this many
// frames queued or fewer, another one is requested on each tick, and below the minimum one more is
// requested to catch up (the system clock and the audio clock do not match exactly).
constexpr u64 kPumpIntervalNs = 1'000'000'000ull * SwitchAudioDriver::kChannelSamples / 48000;
constexpr size_t kTargetQueuedFrames = 6;
constexpr size_t kMinQueuedFrames = 2;

}  // namespace

/* --- driver ------------------------------------------------------------------ */

SwitchAudioDriver::SwitchAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SwitchAudioDriver::~SwitchAudioDriver() {
  std::lock_guard<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SwitchAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  // Runs inside the guest callback while it holds the global critical region:
  // copy and return, never block on the device.
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[kFrameSamples];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }
  std::memcpy(output_frame, input_frame, kFrameSamples * sizeof(float));
  std::lock_guard<std::mutex> guard(frames_mutex_);
  frames_queued_.push(output_frame);
}

bool SwitchAudioDriver::MixFrameInto(float* stereo_out, const StereoFold& fold, float gain,
                                     bool release) {
  float* frame;
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    if (frames_queued_.empty()) {
      return false;
    }
    frame = frames_queued_.front();
    frames_queued_.pop();
  }
  std::array<float, kChannelSamples * 2> folded;
  conversion::sequential_6_BE_to_interleaved_2_LE(folded.data(), frame, kChannelSamples, fold,
                                                  gain);
  for (size_t i = 0; i < folded.size(); ++i) {
    stereo_out[i] += folded[i];
  }
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    frames_unused_.push(frame);
  }
  // Exactly one release per frame really consumed: that is what lets the guest
  // render the next one. With audio_switch_pump, PumpTick requests them at a fixed rate.
  if (release) {
    auto released = semaphore_->Release(1, nullptr);
    assert_true(released);
  }
  return true;
}

size_t SwitchAudioDriver::QueuedFrames() {
  std::lock_guard<std::mutex> guard(frames_mutex_);
  return frames_queued_.size();
}

void SwitchAudioDriver::RequestFrame() {
  // The semaphore has a maximum: if the game does not serve the requests, the excess ones are lost.
  semaphore_->Release(1, nullptr);
}

/* --- system ------------------------------------------------------------------ */

struct SwitchAudioSystem::Output {
  Thread thread{};
  bool thread_created = false;
  std::atomic<bool> running{false};
  bool pump = false;  // audio_switch_pump, read when audio is opened
  size_t frames_in_queue = kTargetQueuedFrames;  // audio_switch_frames_in_queue, read when audio is opened
  bool audout_initialized = false;
  bool audout_started = false;
  std::array<AudioOutBuffer, kBufferCount> buffers{};
  std::array<s16*, kBufferCount> samples{};
  std::array<float, kBufferSamples * 2> mix{};
};

std::unique_ptr<AudioSystem> SwitchAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<SwitchAudioSystem>(function_dispatcher);
}

SwitchAudioSystem::SwitchAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

SwitchAudioSystem::~SwitchAudioSystem() {
  StopOutput();
}

X_STATUS SwitchAudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                         rex::thread::Semaphore* semaphore,
                                         AudioDriver** out_driver) {
  assert_not_null(out_driver);
  // The device opens with the first client and then stays open, playing
  // silence while nobody renders.
  if (!output_ && !StartOutput()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  auto driver = new SwitchAudioDriver(memory_, semaphore);
  {
    std::lock_guard<std::mutex> guard(drivers_mutex_);
    drivers_.push_back(driver);
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void SwitchAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  auto switch_driver = static_cast<SwitchAudioDriver*>(driver);
  {
    std::lock_guard<std::mutex> guard(drivers_mutex_);
    drivers_.erase(std::remove(drivers_.begin(), drivers_.end(), switch_driver), drivers_.end());
  }
  delete switch_driver;
}

bool SwitchAudioSystem::StartOutput() {
  auto output = std::make_unique<Output>();
  output->pump = REXCVAR_GET(audio_switch_pump);
  output->frames_in_queue = static_cast<size_t>(std::clamp(REXCVAR_GET(audio_switch_frames_in_queue), 2, 32));
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("SwitchAudioSystem: 187.5 Hz pump {}, {} frames queued", output->pump ? "active" : "off",
              output->frames_in_queue);

  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: audoutInitialize failed: 0x{:08X}", rc);
    return false;
  }
  output->audout_initialized = true;
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("SwitchAudioSystem: audout {} Hz, {} channels, PCM format {}",
              audoutGetSampleRate(), audoutGetChannelCount(), int(audoutGetPcmFormat()));
  if (audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2 ||
      audoutGetPcmFormat() != PcmFormat_Int16) {
    REXLOG_ERROR("SwitchAudioSystem: unexpected audout format");
    audoutExit();
    return false;
  }

  for (size_t i = 0; i < kBufferCount; ++i) {
    output->samples[i] = static_cast<s16*>(std::aligned_alloc(0x1000, kBufferAllocBytes));
    if (!output->samples[i]) {
      REXLOG_ERROR("SwitchAudioSystem: out of memory for audio buffers");
      output_ = std::move(output);
      StopOutput();
      return false;
    }
    std::memset(output->samples[i], 0, kBufferAllocBytes);
    AudioOutBuffer& buffer = output->buffers[i];
    buffer.next = nullptr;
    buffer.buffer = output->samples[i];
    buffer.buffer_size = kBufferAllocBytes;
    buffer.data_size = kBufferBytes;
    buffer.data_offset = 0;
  }

  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: audoutStartAudioOut failed: 0x{:08X}", rc);
    output_ = std::move(output);
    StopOutput();
    return false;
  }
  output->audout_started = true;

  output_ = std::move(output);
  output_->running = true;
  rc = threadCreate(
      &output_->thread,
      [](void* arg) { static_cast<SwitchAudioSystem*>(arg)->OutputThreadMain(); }, this, nullptr,
      kOutputThreadStack, kOutputThreadPriority, -2);
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: threadCreate failed: 0x{:08X}", rc);
    output_->running = false;
    StopOutput();
    return false;
  }
  output_->thread_created = true;
  rc = threadStart(&output_->thread);
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: threadStart failed: 0x{:08X}", rc);
    output_->running = false;
    StopOutput();
    return false;
  }
  return true;
}

void SwitchAudioSystem::StopOutput() {
  if (!output_) {
    return;
  }
  output_->running = false;
  if (output_->thread_created) {
    threadWaitForExit(&output_->thread);
    threadClose(&output_->thread);
  }
  if (output_->audout_started) {
    audoutStopAudioOut();
  }
  if (output_->audout_initialized) {
    audoutExit();
  }
  for (s16* samples : output_->samples) {
    std::free(samples);
  }
  output_.reset();
}

void SwitchAudioSystem::FillAndAppend(size_t buffer_index) {
  Output& output = *output_;
  XmaDiagReport();  // tw2de (2026-10-21)
  output.mix.fill(0.0f);
  // Snapshot once, so a change mid-buffer cannot split it across two mixes.
  const StereoFold fold = GetStereoFold();
  const float gain = GetOutputGain();
  uint32_t frames_mixed = 0;
  uint32_t frames_missing = 0;
  {
    std::lock_guard<std::mutex> guard(drivers_mutex_);
    for (uint32_t frame = 0; frame < kFramesPerBuffer; ++frame) {
      float* out = output.mix.data() + frame * SwitchAudioDriver::kChannelSamples * 2;
      for (SwitchAudioDriver* driver : drivers_) {
        // A client with nothing queued contributes silence and gets no
        // release, exactly as the SDL driver does on underrun.
        if (driver->MixFrameInto(out, fold, gain, !output.pump)) {
          ++frames_mixed;
        } else {
          ++frames_missing;
        }
      }
    }
  }
  s16* samples = output.samples[buffer_index];
  bool pcm_nonzero = false;
  // How much the mix saturates before it is clipped (the crackling in handheld mode). Counting only.
  float pico = 0.0f;
  uint32_t saturated = 0;
  if (REXCVAR_GET(audio_mute)) {
    std::memset(samples, 0, kBufferBytes);
  } else {
    for (size_t i = 0; i < output.mix.size(); ++i) {
      const float value = std::isfinite(output.mix[i]) ? output.mix[i] : 0.0f;
      const float magnitude = std::fabs(value);
      pico = std::max(pico, magnitude);
      saturated += magnitude > 1.0f ? 1 : 0;
      samples[i] = static_cast<s16>(std::lrint(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
      pcm_nonzero |= samples[i] != 0;
    }
  }
  {  // TW2 (2026-11-13, audio_switch_record_s)
    static const int record_s = REXCVAR_GET(audio_switch_record_s);
    if (record_s > 0) {
      static std::vector<s16> recorded;
      static size_t wanted = 0;
      static bool written = false;
      if (!wanted) {
        wanted = size_t(record_s) * 48000 * 2;
        recorded.reserve(wanted + kBufferBytes / sizeof(s16));
      }
      // How many buffers the device still holds when this one is appended: 0 = it ran dry (it played silence
      // until now), which no count of the game's frames shows.
      static u32 dry = 0, least = 99;
      static std::vector<float> dry_at;
      if (!written) {
        u32 held = 0;
        if (R_SUCCEEDED(audoutGetAudioOutBufferCount(&held))) {
          least = std::min(least, held);
          if (held == 0 && !recorded.empty()) {
            ++dry;
            if (dry_at.size() < 40)
              dry_at.push_back(float(recorded.size() / 2) / 48000.0f);
          }
        }
        recorded.insert(recorded.end(), samples, samples + kBufferBytes / sizeof(s16));
        g_record_seconds = float(recorded.size() / 2) / 48000.0f;
        if (recorded.size() >= wanted) {
          written = true;
          const std::string path = std::string(RexSwitchLogDir()) + "audio_out.raw";
          if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(recorded.data(), sizeof(s16), recorded.size(), f);
            std::fclose(f);
          }
          REXLOG_INFO("[audio] {} s of the output written to {} ({} frames of {} samples a buffer)", record_s, path,
                      int(kFramesPerBuffer), int(SwitchAudioDriver::kChannelSamples));
          std::string at;
          for (float t : dry_at)
            at += fmt::format(" {:.2f}", t);
          g_record_seconds = -1.0f;
          std::string twice, none;
          for (float t : g_record_double)
            twice += fmt::format(" {:.2f}", t);
          for (float t : g_record_none)
            none += fmt::format(" {:.2f}", t);
          REXLOG_INFO("[audio] pump: two frames asked for in one tick at seconds:{}", twice);
          REXLOG_INFO("[audio] pump: no frame asked for (the queue long enough) at seconds:{}", none);
          REXLOG_INFO("[audio] the device held {} buffers at the least when one was appended; it had run dry {} times, at "
                      "seconds:{}", least, dry, at);
          recorded.clear();
          recorded.shrink_to_fit();
        }
      }
    }
  }
  RexSwitchPerfAdd(21, saturated);
  RexSwitchPerfAdd(22, pico >= 0.98f ? 1 : 0);
  RexSwitchPerfMax(23, static_cast<u64>(std::min(pico, 100.0f) * 10000.0f));
  output.buffers[buffer_index].data_size = kBufferBytes;
  output.buffers[buffer_index].data_offset = 0;
  Result rc = audoutAppendAudioOutBuffer(&output.buffers[buffer_index]);
  RexSwitchPerfAdd(24, frames_mixed);
  RexSwitchPerfAdd(25, frames_missing);
  if (R_SUCCEEDED(rc)) {
    RexSwitchPerfAdd(26, pcm_nonzero ? 1 : 0);
    RexSwitchPerfAdd(27, 1);
  }
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: audoutAppendAudioOutBuffer failed: 0x{:08X}", rc);
  }
}

void SwitchAudioSystem::PumpTick() {
  std::lock_guard<std::mutex> guard(drivers_mutex_);
  for (SwitchAudioDriver* driver : drivers_) {
    const size_t queued = driver->QueuedFrames();
    if (queued <= output_->frames_in_queue) {
      driver->RequestFrame();
    } else if (g_record_seconds >= 0.0f && g_record_none.size() < 60) {
      g_record_none.push_back(g_record_seconds);
    }
    if (queued < kMinQueuedFrames) {
      driver->RequestFrame();  // the queue has emptied: one more to catch up
      if (g_record_seconds >= 0.0f && g_record_double.size() < 60)
        g_record_double.push_back(g_record_seconds);
    }
  }
}

void SwitchAudioSystem::OutputThreadMain() {
  Output& output = *output_;
  for (size_t i = 0; i < kBufferCount; ++i) {
    FillAndAppend(i);
  }
  const u64 interval = armNsToTicks(kPumpIntervalNs);
  u64 deadline = armGetSystemTick() + interval;
  while (output.running.load(std::memory_order_relaxed)) {
    u64 timeout_ns = kWaitTimeoutNs;
    if (output.pump) {
      const u64 now = armGetSystemTick();
      if (now >= deadline) {
        PumpTick();
        deadline += interval;
        if (now >= deadline) {
          deadline = now + interval;  // more than one tick late: no catch-up burst
        }
        continue;
      }
      timeout_ns = armTicksToNs(deadline - now);  // at most until the next deadline
    }
    AudioOutBuffer* released = nullptr;
    u32 released_count = 0;
    Result rc = audoutWaitPlayFinish(&released, &released_count, timeout_ns);
    // libnx hands back one released buffer per call; drain the rest so none is
    // left out of the rotation.
    while (R_SUCCEEDED(rc) && released && released_count) {
      const ptrdiff_t index = released - output.buffers.data();
      if (index >= 0 && index < ptrdiff_t(kBufferCount)) {
        FillAndAppend(size_t(index));
      }
      released = nullptr;
      released_count = 0;
      rc = audoutGetReleasedAudioOutBuffer(&released, &released_count);
    }
  }
}

}  // namespace rex::audio::nx

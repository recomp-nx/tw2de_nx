// tw2de (2026-11-05): frame_timing.h.
//
// What is measured. The display (the Switch's compositor) takes a new image only at a refresh, and with the display's
// swap interval N (frame_interval.cpp: 2 in play) at most every N refreshes. An image can be taken at the first refresh
// after both its present was queued and the GPU finished it. So each frame gets:
//   ready      = the later of "present returned" and "GPU done"
//   deadline   = the refresh N after the refresh the previous frame was taken at
//   taken      = the later of the deadline and the first refresh after 'ready'
// A frame ready after its deadline is a drop: the previous image stays N + 1 (or more) refreshes, although the average
// frame rate hardly moves. A frame ready long before its deadline waits (more latency, and a full swapchain makes the
// next acquire wait). Every time is also given against the deadline, so a drop's line shows which stage crossed it:
// the game's CPU frame (start -> Swap), the render thread (Swap -> submit), the swapchain (acquire, present) or the GPU.
//
// 'taken' is worked out, not seen: the compositor does not say when it took an image. With the display's own interval
// N this is how it behaves (no image before N refreshes, none before it is ready). The refresh times are the vsync
// thread's wake times, a few tens of microseconds after the event.
//
// With tw2de_diag_vsync_timing: every 10 s a [vsync] line (frames, drops, the margin before the deadline as a
// histogram, each stage's mean and worst), and a [vsync] drop line for each late frame (at most 40 in 10 s).
#include "renut_engine/frame_timing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "me/me_platform_compat.h"  // RexSwitchPinCurrentThreadToCore and the priority helper
#include "renut_engine/frame_interval.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"  // StartLongLivedThread
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // Deko3DFenceCopy (NoteAcquire)
#endif

#if REX_PLATFORM_SWITCH
#include <switch.h>  // svcGetInfo: the thread's own CPU time
#endif

REXCVAR_DEFINE_BOOL(tw2de_diag_vsync_timing, false, "Game/Diagnostics",
                    "Diagnostic: each frame's stages (game frame start, Swap, render, acquire, submit, present, GPU "
                    "done) timed against the display's refresh; a [vsync] line every 10 s and one per frame that missed "
                    "its refresh (renut_engine/frame_timing.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de::frame_timing {
namespace {

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// tw2de (2026-11-05, third round): the calling thread's CPU time (ns): the time it actually ran. Against the wall
// time it tells running from not running (blocked on a mutex, or preempted by a higher-priority thread on its core).
uint64_t ThreadCpuNs() {
#if REX_PLATFORM_SWITCH
  u64 ticks = 0;
  if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX)) &&
      R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, UINT64_MAX)))
    return 0;
  return armTicksToNs(ticks);
#else
  return 0;
#endif
}

constexpr uint32_t kRefreshes = 1024;  // 17 s at 60 Hz
constexpr uint32_t kVblanks = 256;
constexpr uint32_t kFrames = 64;
constexpr uint32_t kReportRefreshes = 600;  // 10 s at 60 Hz
constexpr uint32_t kDropLinesPerReport = 40;

struct Refresh {
  uint64_t index = 0;
  uint64_t ns = 0;
};
struct Vblank {
  uint32_t counter = 0;
  uint64_t ns = 0;
  bool valid = false;
};
struct Frame {
  uint64_t swap = 0;
  uint64_t t[kStageCount] = {};
  uint64_t cpu[kStageCount] = {};  // the marking thread's CPU time at each stage
  uint64_t start = 0;  // the vblank that woke the game for this frame
  uint32_t target = 0;  // the vblank this frame's limiter asked for the next one
  uint64_t present_index = 0;
  uint64_t gpu = 0;
  double rt_busy = 0, rt_waited = 0;  // the render thread on the frame's work before its present (ms)
  double rt_waited_acquire = -1;      // blocked (ms, all of the frame) when the present asked for an image
  // tw2de (2026-11-05, progressive submit): NotePieces.
  uint32_t pieces = 0, kicks = 0, draws = 0, tail_draws = 0;
  double submit_ms = 0;
  bool have_pieces = false;
  // tw2de (2026-11-05, sixth round): NoteAcquire.
  uint64_t next = 0;   // the Swap presented after this one
  uint64_t shown = 0;  // the refresh it went on screen at (0: not seen)
};

struct Stat {
  uint64_t n = 0;
  double total = 0, worst = 0;
  void Add(double ms) {
    ++n;
    total += ms;
    worst = std::max(worst, ms);
  }
  std::string Text() const { return n ? fmt::format("{:.2f}/{:.2f}", total / double(n), worst) : std::string("-"); }
};

std::mutex g_mutex;  // everything below
Refresh g_refresh[kRefreshes];
uint64_t g_refresh_last = 0;  // the newest refresh's index (0: none yet)
Vblank g_vblank[kVblanks];
uint32_t g_vblank_next = 0;
Frame g_frame[kFrames];
uint64_t g_next_analysed = 0;  // the next Swap to analyse (0: the first one seen)
uint64_t g_last_taken = 0;     // the refresh the previous frame was taken at
uint32_t g_last_target = 0;    // the previous frame's limiter target (its next frame's start)
bool g_have_target = false;

// The 10 s report.
uint64_t g_frames = 0, g_drops = 0, g_extra = 0, g_unknown = 0, g_drop_lines = 0, g_report_from = 0;
uint64_t g_margin_bins[7] = {};  // late; 0-2; 2-5; 5-10; 10-16.7; 16.7-33.3; more (ms before the deadline)
// The render thread's present closure up to the acquire, in its steps: wall time, and the part it did not run.
Stat g_step_wall[4], g_step_off[4];
Stat g_cpu, g_queue, g_render, g_acquire, g_submit, g_present, g_gpu, g_wake, g_margin, g_rt_busy, g_rt_waited,
    g_rt_present_wait;

std::atomic<uint64_t> g_presenting{0};

// tw2de (2026-11-05, fourth round): the bookkeeping's parts (PartScope, TakeParts).
constexpr const char *kPartNames[kPartCount] = {
    "front buffer", "texture unlocks", "idle textures released", "cold textures released", "stream frame start",
    "[burst] stats", "sweeps (exports, targets, cards)", "stream/index verifiers", "idle streams dropped",
    "resolution latch", "memory report"};
constexpr double kPartsLineMs = 5.0;
constexpr uint32_t kPartsLinesPerReport = 40;
thread_local uint64_t t_part_wall[kPartCount] = {}, t_part_cpu[kPartCount] = {};
Stat g_part_wall[kPartCount], g_part_total;
Stat g_pieces, g_kicks, g_draws, g_tail_draws, g_submit_ms;  // tw2de (2026-11-05, progressive submit)
// tw2de (2026-11-05, sixth round): the frames seen on screen (NoteAcquire).
constexpr uint32_t kSlots = 8;
uint64_t g_slot_swap[kSlots] = {};  // the Swap each swapchain image was last presented for
uint64_t g_last_presented = 0;
uint64_t g_seen_frames = 0, g_seen_unknown = 0, g_seen_bins[5] = {}, g_seen_lines = 0;  // bins: 1, 2, 3, 4, more
struct ReleaseItem {
  uint64_t prev = 0;  // the Swap whose image the acquire took
#if defined(PLUME_DEKO3D)
  plume::Deko3DFenceCopy before, after;
#endif
};
std::mutex g_release_mutex;
std::condition_variable g_release_cv;
std::deque<ReleaseItem> g_release_queue;
uint64_t g_parts_lines = 0;

// The GPU waiter.
std::mutex g_wait_mutex;
std::condition_variable g_wait_cv;
std::deque<std::pair<uint64_t, uint64_t>> g_wait_queue;  // (swap, present index)

Frame &FrameOf(uint64_t swap) { return g_frame[swap % kFrames]; }

// The first refresh at or after 'ns' (its index and time); false when the ring does not reach it. Requires g_mutex.
bool RefreshAtOrAfter(uint64_t ns, uint64_t &index, uint64_t &at) {
  if (!g_refresh_last)
    return false;
  const uint64_t oldest = g_refresh_last > kRefreshes - 1 ? g_refresh_last - (kRefreshes - 1) : 1;
  if (g_refresh[g_refresh_last % kRefreshes].ns < ns)
    return false;  // not yet
  for (uint64_t i = g_refresh_last;; --i) {
    const Refresh &r = g_refresh[i % kRefreshes];
    if (r.index != i)
      return false;
    if (r.ns < ns) {
      const Refresh &next = g_refresh[(i + 1) % kRefreshes];
      index = next.index;
      at = next.ns;
      return true;
    }
    if (i == oldest)
      return false;
  }
}

bool RefreshTime(uint64_t index, uint64_t &at) {
  const Refresh &r = g_refresh[index % kRefreshes];
  if (r.index != index)
    return false;
  at = r.ns;
  return true;
}

// The time the device's vblank counter first reached 'counter'. Requires g_mutex.
uint64_t VblankTime(uint32_t counter) {
  uint64_t best = 0;
  for (const Vblank &v : g_vblank)
    if (v.valid && int32_t(v.counter - counter) >= 0 && (!best || v.ns < best))
      best = v.ns;
  return best;
}

constexpr Stage kSteps[5] = {kRenderBegin, kRenderLocked, kRenderReady, kPresentLocked, kAcquireBegin};
constexpr const char *kStepNames[4] = {"waiting for the video mutex", "the closure's work before the present (the "
                                       "[vsync] parts line has its parts)", "Video::Present's mutex",
                                       "the present before the acquire"};

// A step's wall ms and the ms the thread did not run in it (both 0 when a mark is missing).
void StepMs(const Frame &f, int i, double &wall, double &off) {
  wall = off = 0;
  const Stage a = kSteps[i], b = kSteps[i + 1];
  if (!f.t[a] || !f.t[b] || f.t[b] < f.t[a])
    return;
  wall = double(f.t[b] - f.t[a]) / 1e6;
  if (f.cpu[a] && f.cpu[b] >= f.cpu[a])
    off = std::max(0.0, wall - double(f.cpu[b] - f.cpu[a]) / 1e6);
}

std::string StepsText(const Frame &f) {
  std::string s;
  for (int i = 0; i < 4; ++i) {
    double wall, off;
    StepMs(f, i, wall, off);
    s += fmt::format("{}{} {:.2f} (not running {:.2f})", i ? ", " : "", kStepNames[i], wall, off);
  }
  return s;
}

double Rel(uint64_t t, uint64_t deadline) { return t ? (double(int64_t(t - deadline))) / 1e6 : 0.0; }
double Ms(uint64_t from, uint64_t to) { return from && to > from ? double(to - from) / 1e6 : 0.0; }

// The frames whose image is ready by the newest refresh. Requires g_mutex.
void AnalyseLocked(uint64_t now_refresh_ns) {
  for (;;) {
    if (!g_next_analysed)
      return;
    Frame &f = FrameOf(g_next_analysed);
    if (f.swap != g_next_analysed) {
      // Overwritten or never recorded (a Swap with no present): skip it.
      if (f.swap > g_next_analysed) {
        ++g_unknown;
        ++g_next_analysed;
        continue;
      }
      return;
    }
    if (!f.t[kPresented] || !f.gpu) {
      // Not done yet; a frame never presented (the window minimized, a dropped present) is passed over after 0.5 s.
      if (f.t[kSwap] && now_refresh_ns > f.t[kSwap] + 500'000'000ull) {
        ++g_unknown;
        ++g_next_analysed;
        continue;
      }
      return;
    }
    const uint64_t ready = std::max(f.t[kPresented], f.gpu);
    if (ready > now_refresh_ns)
      return;  // its refresh has not come yet
    uint64_t ready_refresh = 0, ready_refresh_ns = 0;
    if (!RefreshAtOrAfter(ready, ready_refresh, ready_refresh_ns)) {
      ++g_unknown;
      ++g_next_analysed;
      continue;
    }
    const uint32_t interval = std::max<uint32_t>(1, tw2de::frame_interval::SwapInterval(true));
    const uint64_t deadline = g_last_taken ? g_last_taken + interval : ready_refresh;
    const uint64_t taken = std::max(deadline, ready_refresh);
    uint64_t deadline_ns = 0;
    RefreshTime(deadline, deadline_ns);
    ++g_frames;
    const double margin = deadline_ns ? double(int64_t(deadline_ns - ready)) / 1e6 : 0.0;
    g_margin.Add(std::max(0.0, margin));
    const int bin = margin < 0 ? 0 : margin < 2 ? 1 : margin < 5 ? 2 : margin < 10 ? 3 : margin < 16.7 ? 4
                                 : margin < 33.3 ? 5 : 6;
    ++g_margin_bins[bin];
    g_cpu.Add(Ms(f.start, f.t[kSwap]));
    g_queue.Add(Ms(f.t[kSwap], f.t[kRenderBegin]));
    g_render.Add(Ms(f.t[kRenderBegin], f.t[kAcquireBegin]));
    g_acquire.Add(Ms(f.t[kAcquireBegin], f.t[kAcquired]));
    g_submit.Add(Ms(f.t[kAcquired], f.t[kSubmitted]));
    g_present.Add(Ms(f.t[kSubmitted], f.t[kPresented]));
    g_gpu.Add(Ms(f.t[kSubmitted], f.gpu));
    for (int i = 0; i < 4; ++i) {
      double wall, off;
      StepMs(f, i, wall, off);
      g_step_wall[i].Add(wall);
      g_step_off[i].Add(off);
    }
    if (f.have_pieces) {
      g_pieces.Add(f.pieces);
      g_kicks.Add(f.kicks);
      g_draws.Add(f.draws);
      g_tail_draws.Add(f.tail_draws);
      g_submit_ms.Add(f.submit_ms);
    }
    g_rt_busy.Add(f.rt_busy);
    g_rt_waited.Add(f.rt_waited);
    if (f.rt_waited_acquire >= 0)
      g_rt_present_wait.Add(std::max(0.0, f.rt_waited_acquire - f.rt_waited));
    if (f.start) {
      uint64_t wake_refresh = 0, wake_ns = 0;
      // The game's wake against the refresh before it (the vblank pump's delay plus the game's own wake).
      if (RefreshAtOrAfter(f.start, wake_refresh, wake_ns) && wake_refresh > 1) {
        uint64_t before = 0;
        if (RefreshTime(wake_refresh - 1, before))
          g_wake.Add(Ms(before, f.start));
      }
    }
    if (g_last_taken && ready_refresh > deadline) {
      ++g_drops;
      g_extra += ready_refresh - deadline;
      if (g_drop_lines++ < kDropLinesPerReport) {
        // The first stage past the deadline is the one that made the frame late.
        const char *late = "GPU";
        if (f.start > deadline_ns)
          late = "game wake (its limiter woke it after the deadline)";
        else if (f.t[kSwap] > deadline_ns)
          late = "game CPU frame (Swap after the deadline)";
        else if (f.t[kSubmitted] > deadline_ns)
          late = "render thread (submit after the deadline)";
        else if (f.t[kPresented] > deadline_ns)
          late = "swapchain present";
        REXLOG_INFO("[vsync] drop: Swap {} missed refresh {} by {} refresh(es); ms against it: start {:+.2f}, Swap "
                    "{:+.2f}, render {:+.2f}, acquire {:+.2f}..{:+.2f}, submit {:+.2f}, present {:+.2f}, GPU done "
                    "{:+.2f}; CPU frame {:.2f}, GPU {:.2f}; render thread on the frame: busy {:.2f}, blocked {:.2f}, "
                    "blocked in its present before the acquire {:.2f}; the render thread's way to the acquire, ms: {}; "
                    "to the GPU in {} pieces at {} kicks + the present's ({} of {} draws), submits {:.2f} ms; late: {}",
                    f.swap, deadline, ready_refresh - deadline, Rel(f.start, deadline_ns), Rel(f.t[kSwap], deadline_ns),
                    Rel(f.t[kRenderBegin], deadline_ns), Rel(f.t[kAcquireBegin], deadline_ns),
                    Rel(f.t[kAcquired], deadline_ns), Rel(f.t[kSubmitted], deadline_ns),
                    Rel(f.t[kPresented], deadline_ns), Rel(f.gpu, deadline_ns), Ms(f.start, f.t[kSwap]),
                    Ms(f.t[kSubmitted], f.gpu), f.rt_busy, f.rt_waited,
                    f.rt_waited_acquire >= 0 ? std::max(0.0, f.rt_waited_acquire - f.rt_waited) : 0.0, StepsText(f),
                    f.pieces, f.kicks, f.tail_draws, f.draws, f.submit_ms, late);
      }
    }
    g_last_taken = taken;
    ++g_next_analysed;
  }
}

void ReportLocked() {
  REXLOG_INFO("[vsync] {} frames, {} dropped ({} refreshes repeated), {} not measured; ready before the deadline: late "
              "{}, 0-2 ms {}, 2-5 {}, 5-10 {}, 10-16.7 {}, 16.7-33.3 {}, more {} (mean/worst margin {} ms); ms mean/worst: "
              "game wake after its refresh {}, CPU frame {}, Swap -> render thread {}, render {}, acquire wait {}, submit "
              "{}, present call {}, GPU {}; render thread on a frame: busy {}, blocked {}, blocked in the present "
              "before the acquire {}; its way to the acquire, ms mean/worst (and not running): {}",
              g_frames, g_drops, g_extra, g_unknown, g_margin_bins[0], g_margin_bins[1], g_margin_bins[2],
              g_margin_bins[3], g_margin_bins[4], g_margin_bins[5], g_margin_bins[6], g_margin.Text(), g_wake.Text(),
              g_cpu.Text(), g_queue.Text(), g_render.Text(), g_acquire.Text(), g_submit.Text(), g_present.Text(),
              g_gpu.Text(), g_rt_busy.Text(), g_rt_waited.Text(), g_rt_present_wait.Text(), [] {
                std::string s;
                for (int i = 0; i < 4; ++i)
                  s += fmt::format("{}{} {} ({})", i ? ", " : "", kStepNames[i], g_step_wall[i].Text(),
                                   g_step_off[i].Text());
                return s;
              }());
  g_frames = g_drops = g_extra = g_unknown = g_drop_lines = 0;
  std::fill(std::begin(g_margin_bins), std::end(g_margin_bins), 0);
  g_cpu = g_queue = g_render = g_acquire = g_submit = g_present = g_gpu = g_wake = g_margin = g_rt_busy = g_rt_waited =
      g_rt_present_wait = Stat{};
  for (int i = 0; i < 4; ++i)
    g_step_wall[i] = g_step_off[i] = Stat{};
  if (g_part_total.n) {
    std::string parts;
    for (uint32_t i = 0; i < kPartCount; ++i)
      parts += fmt::format("{}{} {}", i ? ", " : "", kPartNames[i], g_part_wall[i].Text());
    REXLOG_INFO("[vsync] the render thread's frame bookkeeping, ms mean/worst: all {}; {}", g_part_total.Text(), parts);
  }
  if (g_pieces.n)
    REXLOG_INFO("[vsync] frames to the GPU, mean/worst: pieces at kicks {}, kicks {}, draws {}, draws in the last list "
                "(sent at the present) {}, the render thread's submits {} ms", g_pieces.Text(), g_kicks.Text(),
                g_draws.Text(), g_tail_draws.Text(), g_submit_ms.Text());
  g_pieces = g_kicks = g_draws = g_tail_draws = g_submit_ms = Stat{};
  if (g_seen_frames || g_seen_unknown)
    REXLOG_INFO("[vsync] seen on screen (the display's real changes, not the model): {} frames measured, {} not "
                "(the image released before the GPU reached the acquire); on screen for 1 refresh {}, 2 {}, 3 {}, 4 {}, "
                "more {}", g_seen_frames, g_seen_unknown, g_seen_bins[0], g_seen_bins[1], g_seen_bins[2],
                g_seen_bins[3], g_seen_bins[4]);
  g_seen_frames = g_seen_unknown = g_seen_lines = 0;
  std::fill(std::begin(g_seen_bins), std::end(g_seen_bins), 0);
  g_part_total = Stat{};
  for (Stat &st : g_part_wall)
    st = Stat{};
  g_parts_lines = 0;
}

// The GPU's end of each presented frame, as it happens (a wait on the frame's fence).
void GpuWaiter() {
  RexSwitchSetCurrentThreadPriorityOk(0x2C);
  RexSwitchPinCurrentThreadToCore(2);
  for (;;) {
    std::pair<uint64_t, uint64_t> item;
    {
      std::unique_lock lock(g_wait_mutex);
      g_wait_cv.wait(lock, [] { return !g_wait_queue.empty(); });
      item = g_wait_queue.front();
      g_wait_queue.pop_front();
    }
    const bool done = renut::gpu::Video::WaitPresentDone(item.second);
    const uint64_t now = NowNs();
    std::lock_guard lock(g_mutex);
    Frame &f = FrameOf(item.first);
    if (f.swap == item.first)
      f.gpu = done ? now : f.t[kPresented];  // not known (too old, no fences): taken as done at the present
  }
}

// tw2de (2026-11-05, sixth round): the refresh nearest 'ns' (within 4 ms). Requires g_mutex.
bool NearestRefresh(uint64_t ns, uint64_t &index) {
  if (!g_refresh_last)
    return false;
  const uint64_t oldest = g_refresh_last > kRefreshes - 1 ? g_refresh_last - (kRefreshes - 1) : 1;
  uint64_t best = 0, best_d = 4'000'000;
  for (uint64_t i = g_refresh_last; i >= oldest; --i) {
    const Refresh &r = g_refresh[i % kRefreshes];
    if (r.index != i)
      break;
    const uint64_t d = r.ns > ns ? r.ns - ns : ns - r.ns;
    if (d < best_d) {
      best_d = d;
      best = i;
    }
    if (r.ns + 20'000'000 < ns)
      break;  // older ones are further away
    if (i == 1)
      break;
  }
  index = best;
  return best != 0;
}

// A release seen: the frame after 'prev' went on screen at refresh 'r'. Requires g_mutex.
void NoteShownLocked(uint64_t prev, uint64_t r) {
  Frame &k = FrameOf(prev);
  if (k.swap != prev || !k.next) {
    ++g_seen_unknown;
    return;
  }
  Frame &y = FrameOf(k.next);
  if (y.swap != k.next) {
    ++g_seen_unknown;
    return;
  }
  y.shown = r;
  if (!k.shown || r <= k.shown)
    return;
  const uint64_t d = r - k.shown;
  ++g_seen_frames;
  ++g_seen_bins[std::min<uint64_t>(d, 5) - 1];
  const uint32_t interval = std::max<uint32_t>(1, tw2de::frame_interval::SwapInterval(true));
  if (d > interval && g_seen_lines++ < kDropLinesPerReport) {
    uint64_t due_ns = 0;
    RefreshTime(k.shown + interval, due_ns);
    REXLOG_INFO("[vsync] seen: Swap {} on screen {} refreshes (from refresh {}); Swap {} went on at refresh {}, due at "
                "{}; its ms against the due refresh: start {:+.2f}, Swap {:+.2f}, render {:+.2f}, acquire {:+.2f}, submit "
                "{:+.2f}, present {:+.2f}, GPU done {:+.2f}; CPU frame {:.2f}; render thread busy {:.2f}; to the GPU in "
                "{} pieces ({} of {} draws at the present)",
                k.swap, d, k.shown, y.swap, r, k.shown + interval, Rel(y.start, due_ns), Rel(y.t[kSwap], due_ns),
                Rel(y.t[kRenderBegin], due_ns), Rel(y.t[kAcquireBegin], due_ns), Rel(y.t[kSubmitted], due_ns),
                Rel(y.t[kPresented], due_ns), Rel(y.gpu, due_ns), Ms(y.start, y.t[kSwap]), y.rt_busy, y.pieces,
                y.tail_draws, y.draws);
  }
}

void ReleaseWaiter() {
  RexSwitchSetCurrentThreadPriorityOk(0x2C);
  RexSwitchPinCurrentThreadToCore(2);
  for (;;) {
    ReleaseItem item;
    {
      std::unique_lock lock(g_release_mutex);
      g_release_cv.wait(lock, [] { return !g_release_queue.empty(); });
      item = g_release_queue.front();
      g_release_queue.pop_front();
    }
#if defined(PLUME_DEKO3D)
    const bool a = plume::Deko3DFenceCopyWait(item.before, nullptr);
    const uint64_t ta = NowNs();
    const bool b = plume::Deko3DFenceCopyWait(item.after, nullptr);
    const uint64_t tb = NowNs();
    std::lock_guard lock(g_mutex);
    uint64_t r = 0;
    // Waited for the release (0.15 ms or more after the work before it): tb is the release, at a vsync.
    if (!a || !b || tb < ta + 150'000 || !NearestRefresh(tb, r)) {
      ++g_seen_unknown;
      continue;
    }
    NoteShownLocked(item.prev, r);
#endif
  }
}

}  // namespace

bool On() {
  static const bool on = REXCVAR_GET(tw2de_diag_vsync_timing);
  return on;
}

void NoteRefresh(uint64_t index, uint64_t ns) {
  if (!On())
    return;
  std::lock_guard lock(g_mutex);
  g_refresh[index % kRefreshes] = {index, ns};
  g_refresh_last = index;
  AnalyseLocked(ns);
  if (!g_report_from)
    g_report_from = index;
  if (index >= g_report_from + kReportRefreshes) {
    g_report_from = index;
    ReportLocked();
  }
}

void NoteVblank(uint32_t counter) {
  if (!On())
    return;
  const uint64_t now = NowNs();
  std::lock_guard lock(g_mutex);
  g_vblank[g_vblank_next] = {counter, now, true};
  g_vblank_next = (g_vblank_next + 1) % kVblanks;
}

void NoteTarget(uint64_t swap, uint32_t target) {
  if (!On())
    return;
  std::lock_guard lock(g_mutex);
  Frame &f = FrameOf(swap);
  if (f.swap == swap)
    f.target = target;
  g_last_target = target;
  g_have_target = true;
}

void Mark(uint64_t swap, Stage stage) {
  if (!On() || !swap)
    return;
  const uint64_t now = NowNs();
  const uint64_t cpu = ThreadCpuNs();
  std::lock_guard lock(g_mutex);
  Frame &f = FrameOf(swap);
  if (stage == kSwap) {
    // A new frame: it started when the device's vblank counter reached the previous frame's target.
    const uint32_t start_target = g_last_target;
    const bool have = g_have_target;
    f = Frame{};
    f.swap = swap;
    f.start = have ? VblankTime(start_target) : 0;
    if (f.start > now)
      f.start = 0;
    if (!g_next_analysed)
      g_next_analysed = swap;
    static std::once_flag once;
    std::call_once(once, [] {
      tw2de::renut_gpu::StartLongLivedThread(&GpuWaiter, 64 * 1024);
      REXLOG_INFO("[vsync] frame timing against the display's refresh (tw2de_diag_vsync_timing)");
    });
  } else if (f.swap != swap) {
    return;
  }
  f.t[stage] = now;
  f.cpu[stage] = cpu;
}

void NoteRenderThread(uint64_t swap, double busy_ms, double waited_ms) {
  if (!On() || !swap)
    return;
  std::lock_guard lock(g_mutex);
  Frame &f = FrameOf(swap);
  if (f.swap == swap) {
    f.rt_busy = busy_ms;
    f.rt_waited = waited_ms;
  }
}

void NoteWaitedBeforeAcquire(uint64_t swap, double waited_ms) {
  if (!On() || !swap)
    return;
  std::lock_guard lock(g_mutex);
  Frame &f = FrameOf(swap);
  if (f.swap == swap)
    f.rt_waited_acquire = waited_ms;
}

void SetPresenting(uint64_t swap) {
  g_presenting.store(swap, std::memory_order_relaxed);
}

uint64_t Presenting() {
  return g_presenting.load(std::memory_order_relaxed);
}

void NotePresentIndex(uint64_t swap, uint64_t present_index) {
  if (!On() || !swap)
    return;
  {
    std::lock_guard lock(g_mutex);
    Frame &f = FrameOf(swap);
    if (f.swap != swap)
      return;
    f.present_index = present_index;
  }
  {
    std::lock_guard lock(g_wait_mutex);
    g_wait_queue.emplace_back(swap, present_index);
  }
  g_wait_cv.notify_one();
  std::lock_guard lock(g_mutex);  // tw2de (2026-11-05, sixth round): the order of the presents (NoteAcquire)
  if (g_last_presented && FrameOf(g_last_presented).swap == g_last_presented)
    FrameOf(g_last_presented).next = swap;
  g_last_presented = swap;
}

void NoteAcquire(uint64_t swap, uint32_t slot, const plume::Deko3DFenceCopy &before, const plume::Deko3DFenceCopy &after) {
#if defined(PLUME_DEKO3D)
  if (!On() || !swap || slot >= kSlots)
    return;
  ReleaseItem item;
  {
    std::lock_guard lock(g_mutex);
    item.prev = g_slot_swap[slot];
    g_slot_swap[slot] = swap;
  }
  if (!item.prev)
    return;
  item.before = before;
  item.after = after;
  static std::once_flag once;
  std::call_once(once, [] { tw2de::renut_gpu::StartLongLivedThread(&ReleaseWaiter, 64 * 1024); });
  {
    std::lock_guard lock(g_release_mutex);
    g_release_queue.push_back(item);
  }
  g_release_cv.notify_one();
#else
  (void)swap, (void)slot, (void)before, (void)after;
#endif
}

PartScope::PartScope(Part part) : part_(part) {
  if (On()) {
    t0_ = NowNs();
    cpu0_ = ThreadCpuNs();
  }
}

PartScope::~PartScope() {
  if (!t0_)
    return;
  const uint64_t cpu = ThreadCpuNs();
  t_part_wall[part_] += NowNs() - t0_;
  if (cpu0_ && cpu >= cpu0_)
    t_part_cpu[part_] += cpu - cpu0_;
}

void TakeParts(uint64_t swap) {
  if (!On())
    return;
  uint64_t wall[kPartCount], cpu[kPartCount], total = 0, total_cpu = 0;
  for (uint32_t i = 0; i < kPartCount; ++i) {
    wall[i] = t_part_wall[i];
    cpu[i] = t_part_cpu[i];
    t_part_wall[i] = t_part_cpu[i] = 0;
    total += wall[i];
    total_cpu += cpu[i];
  }
  std::lock_guard lock(g_mutex);
  g_part_total.Add(double(total) / 1e6);
  for (uint32_t i = 0; i < kPartCount; ++i)
    g_part_wall[i].Add(double(wall[i]) / 1e6);
  if (double(total) / 1e6 >= kPartsLineMs && g_parts_lines++ < kPartsLinesPerReport) {
    std::string parts;
    for (uint32_t i = 0; i < kPartCount; ++i)
      if (wall[i] >= 100'000)  // 0.1 ms
        parts += fmt::format("{}{} {:.2f} (not running {:.2f})", parts.empty() ? "" : ", ", kPartNames[i],
                             double(wall[i]) / 1e6, double(wall[i] > cpu[i] ? wall[i] - cpu[i] : 0) / 1e6);
    REXLOG_INFO("[vsync] parts: Swap {}: the frame bookkeeping took {:.2f} ms (not running {:.2f}): {}", swap,
                double(total) / 1e6, double(total > total_cpu ? total - total_cpu : 0) / 1e6, parts);
  }
}

void NotePieces(uint64_t swap, uint32_t pieces, uint32_t kicks, uint32_t draws, uint32_t tail_draws, double submit_ms) {
  if (!On() || !swap)
    return;
  std::lock_guard lock(g_mutex);
  Frame &f = FrameOf(swap);
  if (f.swap != swap)
    return;
  f.pieces = pieces;
  f.kicks = kicks;
  f.draws = draws;
  f.tail_draws = tail_draws;
  f.submit_ms = submit_ms;
  f.have_pieces = true;
}

}  // namespace tw2de::frame_timing

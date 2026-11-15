/**
 * @file    gpu/backend/present.cpp
 * @brief   End of frame: swapchain acquire, guest image scan-out,
 *          submit and present.
 */
#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>

#include <plume_render_interface.h>

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/compile_notice.h"  // tw2de (2026-10-22)
#include "renut_engine/gpu/backend/achievement_popup.h"
#include <cmath>  // tw2de (2026-10-23)
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/graphics_options.h"  // tw2de (2026-11-07): Output1080p
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/backend/video_internal.h"
#include "renut_engine/gpu/tw2de_renut_capture.h"
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // tw2de: tw2de_renut_capture_raw's format check
#endif

// tw2de (2026-10-17, deko3d renderer tests): the captures copy the game's frame texture as it is (4-byte formats only,
// its channels as BGRA), without the scan-out pass: which of the two a wrong capture comes from.
REXCVAR_DEFINE_BOOL(tw2de_renut_capture_raw, false, "Game/Renut",
                    "Test: frame captures copy the game's frame texture itself instead of the scanned-out picture "
                    "(deko3d builds, 4-byte formats)");

// tw2de (2026-10-18, the deko3d depth of field): one of the game's textures on screen and in the captures in place of
// the frame, to compare a pass's inputs between the back ends (the [trace] resolve lines name the headers).
REXCVAR_DEFINE_STRING(tw2de_renut_diag_show_texture, "", "Game/Renut",
                      "Diagnostic: show the host texture of this guest texture header (hex address, as the [trace] "
                      "resolve lines name them) instead of the frame, on screen and in captures");
REXCVAR_DEFINE_DOUBLE(tw2de_renut_diag_show_gamma, 1.0, "Game/Renut",
                      "Diagnostic: tw2de_renut_diag_show_texture's exponent (depth near 1 shows with 64 or so)");

// tw2de (2026-10-20): see GameGamma below.
REXCVAR_DEFINE_BOOL(tw2de_renut_game_gamma, false, "Game/Renut",
                    "Apply the gamma ramp the game loads to the output, as the Xbox 360's display and tw2de's own "
                    "renderer do (N&B's darkens the midtones, about x^1.17). Off: Renut's output as before");

REXCVAR_DEFINE_BOOL(renut_vsync, true, "Nuts&Bolts/Graphics",
                    "Show each frame at the display's refresh: no tearing, and the game runs at "
                    "the refresh rate (frame_cap then only limits below it). Off shows frames at "
                    "once, with tearing.");

#include "renut_engine/frame_interval.h"  // tw2de (2026-10-25)
#include "renut_engine/frame_timing.h"    // tw2de (2026-11-05, tw2de_diag_vsync_timing)
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

#if defined(__SWITCH__)
extern "C" void RexSwitchPerfResolution(unsigned w, unsigned h);  // sdk/src/ui/switch_perf.cpp
extern "C" void RexStallDumpNote(const char *note);               // sdk/src/ui/switch_perf.cpp
#endif
REXCVAR_DECLARE(int32_t, tw2de_renut_upscale_filter);  // render/draw.cpp
REXCVAR_DEFINE_INT32(tw2de_diag_dump_threads_at, 0, "Game/Diagnostics",
                     "Diagnostic (2026-11-07): every thread's stack into rex_stall.log at this many seconds after the "
                     "first present, and every 10 s after it, 4 times (0 off): what a loading screen that never ends waits on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {

namespace {

// renut_vsync onto the swapchain (both backends start with it on). Vulkan
// makes the change by recreating the swapchain, which needsResize asks for.
// tw2de (2026-10-25): and the frame interval (frame_interval.h: the boot menu's, 1 while a movie plays).
void ApplyVsync(VideoState &s) {
  static uint32_t applied = 1;
  const uint32_t interval = tw2de::frame_interval::SwapInterval(REXCVAR_GET(renut_vsync));
  if (interval == applied)
    return;
  s.swap_chain->setSwapInterval(interval);
  applied = interval;
  if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("Swap interval {}", interval);
}

void RebuildSwapChain(VideoState &s) {
  // Submit rather than abandon an open list: plume tracks layouts on record.
  if (s.list_open)
    detail::SubmitAndAdvance(s, nullptr, nullptr);
  detail::WaitAllSubmitted(s);
  s.swap_framebuffers.clear();
  if (!s.swap_chain->resize() || !detail::BuildSwapChainTargets(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      RGPU_ERROR("Swapchain resize failed");  // 0x0 while minimized is expected
  }
}

// The most of the game's frame trimmed off either axis to fill the output.
constexpr float kMaxTrim = 0.15f;

struct FrameFit {
  plume::RenderViewport viewport;
  float visible[2] = {1.0f, 1.0f};  // the share of the frame shown on each axis
};

// The frame fills the output (a window that is not the frame's shape, say a
// maximized one) without bars: it covers it, keeping its proportions, and the
// part that overflows is trimmed evenly from both edges. Past kMaxTrim, the
// rest is fitted with bars.
FrameFit FitFrame(uint32_t out_w, uint32_t out_h, uint32_t src_w, uint32_t src_h) {
  FrameFit fit;
  fit.viewport = plume::RenderViewport(0.0f, 0.0f, float(out_w), float(out_h));
  if (src_w == 0 || src_h == 0 || out_w == 0 || out_h == 0)
    return fit;
  const float out_aspect = float(out_w) / float(out_h);
  float src_aspect = float(src_w) / float(src_h);
  if (src_aspect > out_aspect) {
    fit.visible[0] = std::max(out_aspect / src_aspect, 1.0f - kMaxTrim);
    src_aspect *= fit.visible[0];
  } else {
    fit.visible[1] = std::max(src_aspect / out_aspect, 1.0f - kMaxTrim);
    src_aspect /= fit.visible[1];
  }
  float w = float(out_w), h = float(out_h);
  if (src_aspect > out_aspect)
    h = w / src_aspect;
  else
    w = h * src_aspect;
  fit.viewport = plume::RenderViewport((float(out_w) - w) * 0.5f, (float(out_h) - h) * 0.5f, w, h);
  return fit;
}

// tw2de (2026-10-20, tw2de_renut_game_gamma): the gamma ramp the game loads, which the 360's display applies to
// every 8-bit value it scans out (N&B's is about x^1.17: entries [64] = 192, [128] = 459, [255] = 1019 of 1023) and
// Renut's present never did. Off by default (it changes the picture); on, the present looks the ramp up by the value.
struct GameGamma {
  std::mutex mutex;
  std::array<uint16_t, 768> ramp{};  // 10-bit entries: red, green, blue
  uint32_t version = 0;
  // Render thread.
  uint32_t uploaded = 0;
  std::unique_ptr<plume::RenderTexture> texture;  // 256 x 1, RGBA32F
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = kInvalidDescriptor;
  float param = 0.0f;  // UtilityPush::param1 of the scan-outs: the slot as raw bits, 0 = no ramp
};
GameGamma g_gamma;

// Before the frame's passes (a copy may not be recorded inside one). Caller holds s.mutex.
void PrepareGameGammaLocked(VideoState &s) {
  g_gamma.param = 0.0f;
  if (!REXCVAR_GET(tw2de_renut_game_gamma))
    return;
  std::array<uint16_t, 768> ramp;
  uint32_t version;
  {
    std::lock_guard lock(g_gamma.mutex);
    ramp = g_gamma.ramp;
    version = g_gamma.version;
  }
  if (!version || !Video::EnsureCommandListLocked())
    return;
  constexpr plume::RenderFormat kFormat = plume::RenderFormat::R32G32B32A32_FLOAT;
  if (!g_gamma.texture) {
    g_gamma.texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(256, 1, 1, kFormat));
    if (!g_gamma.texture)
      return;
    g_gamma.texture->setName("tw2de game gamma ramp");
    g_gamma.view = g_gamma.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kFormat));
    g_gamma.slot = g_gamma.view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
    if (g_gamma.slot == kInvalidDescriptor) {
      g_gamma.view.reset();
      g_gamma.texture.reset();
      return;
    }
    Video::SetTextureSlotLocked(g_gamma.slot, g_gamma.texture.get(), g_gamma.view.get());
    g_gamma.uploaded = 0;
  }
  // Every frame (4 KB): a frame's list can be dropped at start-up (run 78's first upload never arrived: black output),
  // and the copy is cheaper than tracking that.
  {
    UploadAllocation staging = s.uploads[s.frame].Allocate(256 * 16, 512);
    if (!staging)
      return;
    auto *texels = static_cast<float *>(staging.cpu);
    for (uint32_t i = 0; i < 256; ++i) {
      texels[i * 4 + 0] = float(ramp[i]) / 1023.0f;
      texels[i * 4 + 1] = float(ramp[256 + i]) / 1023.0f;
      texels[i * 4 + 2] = float(ramp[512 + i]) / 1023.0f;
      texels[i * 4 + 3] = 1.0f;
    }
    plume::RenderCommandList *cmd = s.command_list;
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(g_gamma.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(g_gamma.texture.get(), 0, 0),
                           plume::RenderTextureCopyLocation::PlacedFootprint(staging.ref.ref, kFormat, 256, 1, 1, 256,
                                                                             staging.ref.offset),
                           0, 0, 0, nullptr);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(g_gamma.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    if (g_gamma.uploaded != version) {
      REXLOG_INFO("[renut] game gamma ramp applied (tw2de_renut_game_gamma): [0]={}/{}/{} [64]={}/{}/{} [128]={}/{}/{} "
                  "[255]={}/{}/{}", ramp[0], ramp[256], ramp[512], ramp[64], ramp[320], ramp[576], ramp[128], ramp[384],
                  ramp[640], ramp[255], ramp[511], ramp[767]);
    }
    g_gamma.uploaded = version;
  }
  g_gamma.param = std::bit_cast<float>(g_gamma.slot);  // the slot's bits (present_ps.hlsl); never 0, a dynamic slot
}

void RecordScanOut(VideoState &s, const PresentImage &image, uint32_t out_w, uint32_t out_h) {
  plume::RenderCommandList *cmd = s.command_list;
  if (image.layout && *image.layout != plume::RenderTextureLayout::SHADER_READ) {
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(image.texture, plume::RenderTextureLayout::SHADER_READ));
    *image.layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const uint32_t src_w = image.source_width ? image.source_width : image.width;
  const uint32_t src_h = image.source_height ? image.source_height : image.height;
  const FrameFit fit = FitFrame(out_w, out_h, src_w, src_h);
  const plume::RenderViewport &vp = fit.viewport;
  cmd->setViewports(vp);
  cmd->setScissors(plume::RenderRect(int32_t(vp.x), int32_t(vp.y), int32_t(vp.x + vp.width),
                                     int32_t(vp.y + vp.height)));

  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  // tw2de (2026-11-07): a scan-out that upscales (a 1280x720 scene's picture to a 1920x1080 output) with the boot
  // menu's Upscaler (tw2de_renut_upscale_filter 1 Lanczos-2, 3 FSR 1's EASU); the rest bilinear.
  plume::RenderPipeline *pipeline = s.present_pipeline.get();
  if (vp.width > float(src_w) * 1.05f && image.gamma <= 0.0f) {
    static const int32_t setting = REXCVAR_GET(tw2de_renut_upscale_filter);
    // tw2de (2026-11-08): prerendered movies always with Lanczos-2 (FSR 1 gave them random hitches).
    const int32_t filter = setting == 3 && tw2de::frame_interval::MoviePlaying() ? 1 : setting;
    if (filter == 1 && s.present_lanczos_pipeline)
      pipeline = s.present_lanczos_pipeline.get();
    else if (filter == 3 && s.present_easu_pipeline)
      pipeline = s.present_easu_pipeline.get();
  }
  cmd->setPipeline(pipeline);
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);

  UtilityPush push{};
  // The frame's share of the texture, less what the fit trims from its edges.
  const float frame_u = image.width ? float(src_w) / float(image.width) : 1.0f;
  const float frame_v = image.height ? float(src_h) / float(image.height) : 1.0f;
  push.scale[0] = frame_u * fit.visible[0];
  push.scale[1] = frame_v * fit.visible[1];
  push.translate[0] = frame_u * (1.0f - fit.visible[0]) * 0.5f;
  push.translate[1] = frame_v * (1.0f - fit.visible[1]) * 0.5f;
  push.texture_index = image.descriptor;
  push.sampler_index = kLinearClampSamplerSlot;
  push.param0 = image.gamma > 0.0f ? image.gamma : s.output_gamma;
  push.param1 = image.gamma > 0.0f ? 0.0f : g_gamma.param;  // tw2de: not on tw2de_renut_diag_show_texture's views
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
}

// tw2de: the test captures (tw2de_renut_capture.h). CaptureFrame asks; the next Present, minimized or not, scans
// the guest image out once more into an offscreen target of the frame's size and reads it back.
struct FrameCapture {
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<uint64_t> requested{0};
  uint64_t served = 0;  // under mutex
  bool ok = false;
  rex::ui::RawImage image;
  // Render thread only. Kept between captures of one size (the recording takes one per frame); never in flight
  // between them, as each capture waits for the GPU.
  std::unique_ptr<plume::RenderTexture> target;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  std::unique_ptr<plume::RenderBuffer> readback;
  uint32_t width = 0, height = 0, row_texels = 0;
  uint64_t served_render = 0;
};
FrameCapture g_capture;

// The present pipeline's format (kSwapChainFormat in video.cpp).
constexpr plume::RenderFormat kCaptureFormat = plume::RenderFormat::B8G8R8A8_UNORM;

bool CaptureFrameLocked(VideoState &s, const PresentImage &image, rex::ui::RawImage &out) {
  const uint32_t w = image.source_width ? image.source_width : image.width;
  const uint32_t h = image.source_height ? image.source_height : image.height;
  if (!w || !h || !Video::EnsureCommandListLocked())
    return false;
  FrameCapture &c = g_capture;
  if (!c.target || c.width != w || c.height != h) {
    c.framebuffer.reset();
    c.target.reset();
    c.readback.reset();
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(w, h, 1, kCaptureFormat);
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
    c.target = s.device->createTexture(desc);
    if (!c.target)
      return false;
    const plume::RenderTexture *attachment = c.target.get();
    c.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1));
    c.row_texels = (w + 63) & ~63u;  // D3D12 copies rows of 256-byte multiples
    c.readback = s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(c.row_texels) * h * 4));
    if (!c.framebuffer || !c.readback) {
      c.framebuffer.reset();
      c.target.reset();
      c.readback.reset();
      return false;
    }
    c.width = w;
    c.height = h;
  }
  plume::RenderCommandList *cmd = s.command_list;
  bool raw = false;
  bool raw_rgba = false;  // the frame texture's bytes are R G B A (else B G R A, as the scan-out's readback)
#if defined(PLUME_DEKO3D)
  if (REXCVAR_GET(tw2de_renut_capture_raw) && IsDeko3D()) {
    const auto *t = static_cast<const plume::Deko3DTexture *>(image.texture);
    if (t && t->valid) {
      switch (t->desc.format) {
      case plume::RenderFormat::R8G8B8A8_TYPELESS:
      case plume::RenderFormat::R8G8B8A8_UNORM:
        raw = raw_rgba = true;
        break;
      case plume::RenderFormat::B8G8R8A8_UNORM:
        raw = true;
        break;
      default:
        break;
      }
    }
    static int logged = 0;
    if (t && logged++ < 4)
      RGPU_INFO("tw2de_renut_capture_raw: frame texture {}x{} format {}{}", t->desc.width, t->desc.height,
                int(t->desc.format), raw ? "" : ": not 8-bit RGBA or BGRA, the scan-out is captured");
  }
#endif
  if (raw) {
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(image.texture, plume::RenderTextureLayout::COPY_SOURCE));
    if (image.layout)
      *image.layout = plume::RenderTextureLayout::COPY_SOURCE;
    const plume::RenderBox box(0, 0, int32_t(w), int32_t(h));
    cmd->copyTextureRegion(
        plume::RenderTextureCopyLocation::PlacedFootprint(c.readback.get(), kCaptureFormat, w, h, 1, c.row_texels),
        plume::RenderTextureCopyLocation::Subresource(image.texture), 0, 0, 0, &box);
  } else {
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(c.target.get(), plume::RenderTextureLayout::COLOR_WRITE));
  cmd->setFramebuffer(c.framebuffer.get());
  RecordScanOut(s, image, w, h);
  cmd->setFramebuffer(nullptr);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(c.target.get(), plume::RenderTextureLayout::COPY_SOURCE));
  cmd->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(c.readback.get(), kCaptureFormat, w, h, 1, c.row_texels),
      plume::RenderTextureCopyLocation::Subresource(c.target.get()));
  }
  // Submits the frame's work with the capture and waits; the next list starts with nothing bound.
  detail::SubmitAndWaitLocked(s);
  const auto *data = static_cast<const uint8_t *>(c.readback->map());
  if (!data)
    return false;
  out.width = w;
  out.height = h;
  out.stride = size_t(w) * 4;
  out.data.resize(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t *row = data + size_t(y) * c.row_texels * 4;
    uint8_t *dest = out.data.data() + size_t(y) * w * 4;
    // B G R A -> R G B X; a raw RGBA frame texture is in order already (before 2026-10-17 it was swapped too: every
    // raw capture of the Ryujinx runs had red and blue exchanged, the screen itself did not).
    const uint32_t r = raw_rgba ? 0 : 2, b = raw_rgba ? 2 : 0;
    for (uint32_t x = 0; x < w; ++x) {
      dest[x * 4 + 0] = row[x * 4 + r];
      dest[x * 4 + 1] = row[x * 4 + 1];
      dest[x * 4 + 2] = row[x * 4 + b];
      dest[x * 4 + 3] = 0xFF;
    }
  }
  c.readback->unmap();
  return true;
}

// From PresentLocked: one relaxed load when nobody asked.
void ServeCaptureLocked(VideoState &s, const PresentImage *image) {
  FrameCapture &c = g_capture;
  const uint64_t requested = c.requested.load(std::memory_order_acquire);
  if (requested == c.served_render || !image || !image->texture || image->descriptor == kInvalidDescriptor)
    return;
  rex::ui::RawImage out;
  const bool ok = CaptureFrameLocked(s, *image, out);
  c.served_render = requested;
  {
    std::lock_guard lock(c.mutex);
    c.image = std::move(out);
    c.ok = ok;
    c.served = requested;
  }
  c.cv.notify_all();
}

// tw2de_renut_diag_show_texture: the texture to show instead of the frame, when it has a 2D host texture and slot.
bool DiagTextureImageLocked(PresentImage &out) {
  static const u32 va = [] {
    const std::string v = REXCVAR_GET(tw2de_renut_diag_show_texture);
    return v.empty() ? 0u : u32(std::strtoul(v.c_str(), nullptr, 16));
  }();
  if (!va)
    return false;
  HostTexture *t = FindHostTextureLocked(va);
  if (!t || !t->texture || t->slot == UINT32_MAX || t->slot == kInvalidDescriptor ||
      t->dimension != TextureDimension::Tex2D || t->stacked)
    return false;
  out.texture = t->texture.get();
  out.layout = &t->layout;
  out.descriptor = t->slot;
  out.width = t->alloc_width;
  out.height = t->alloc_height;
  out.gamma = float(REXCVAR_GET(tw2de_renut_diag_show_gamma));
  return true;
}

// tw2de (2026-10-22): Video::LastOutputSize.
std::atomic<uint64_t> g_last_output_size{0};

// Records the back-buffer pass and presents. Caller holds s.mutex.
void PresentLocked(VideoState &s, const PresentImage *image) {
  // Logs, once, why the GPU stopped if an earlier submission faulted.
  ReportDeviceLost(s.device.get());
  PresentImage diag_image;
  if (image && DiagTextureImageLocked(diag_image))
    image = &diag_image;
  ApplyVsync(s);
#if defined(PLUME_DEKO3D)
  // tw2de (2026-11-07, tw2de_test_output_1080p): the window at the output's size (1920x1080 docked).
  if (IsDeko3D()) {
    static bool applied_1080p = false;
    if (Output1080p() != applied_1080p) {
      applied_1080p = Output1080p();
      plume::Deko3DSetSwapChainSize(s.swap_chain.get(), applied_1080p ? 1920 : 1280, applied_1080p ? 1080 : 720);
      s.resize_requested.store(true, std::memory_order_release);
    }
  }
#endif
  if (s.swap_chain->needsResize() || s.resize_requested.exchange(false, std::memory_order_acq_rel))
    RebuildSwapChain(s);
  PrepareGameGammaLocked(s);     // tw2de (2026-10-20, tw2de_renut_game_gamma)
  ServeCaptureLocked(s, image);  // tw2de: test captures (tw2de_renut_capture.h)
  if (s.swap_framebuffers.empty()) {
    // Minimized or failed resize: keep the frame ring moving without a present.
    if (s.list_open)
      detail::SubmitAndAdvance(s, nullptr, nullptr);
    return;
  }

  namespace ft = tw2de::frame_timing;  // tw2de (2026-11-05): the stages against the refresh (frame_timing.h)
  const uint64_t timed_swap = ft::Presenting();
  ft::Mark(timed_swap, ft::kAcquireBegin);
  if (ft::On())
    ft::NoteWaitedBeforeAcquire(timed_swap, render_thread::WaitedMsThisFrame());
  plume::RenderCommandSemaphore *acquire = s.acquire_semaphores[s.frame].get();
#if defined(PLUME_DEKO3D)
  // tw2de (2026-11-05, sixth round): a fence before the acquire and one after it, the display's real changes
  // (frame_timing::NoteAcquire). Only with tw2de_diag_vsync_timing.
  const bool seen_probe = ft::On() && IsDeko3D();
  plume::Deko3DFenceCopy seen_before;
  if (seen_probe)
    seen_before = plume::Deko3DSignalNow(s.device.get());
#endif
  uint32_t image_index = 0;
  auto acquire_image = [&] {
    render_thread::WaitScope wait;
    return s.swap_chain->acquireTexture(acquire, &image_index);
  };
  if (!acquire_image()) {
    RebuildSwapChain(s);
    acquire = s.acquire_semaphores[s.frame].get();
    if (s.swap_framebuffers.empty() || !acquire_image()) {
      if (s.list_open)
        detail::SubmitAndAdvance(s, nullptr, nullptr);
      return;
    }
  }

  ft::Mark(timed_swap, ft::kAcquired);
#if defined(PLUME_DEKO3D)
  if (seen_probe)
    ft::NoteAcquire(timed_swap, image_index, seen_before, plume::Deko3DSignalNow(s.device.get()));
#endif
  if (!Video::EnsureCommandListLocked())
    return;
  static const uint32_t kProfileLabel = gpu_profile::Label("present");
  gpu_profile::Mark(s, kProfileLabel);
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderTexture *back = s.swap_chain->getTexture(image_index);
  plume::RenderFramebuffer *back_fb = s.swap_framebuffers[image_index].get();
  const uint32_t out_w = s.swap_chain->getWidth();
  const uint32_t out_h = s.swap_chain->getHeight();
  g_last_output_size.store(uint64_t(out_w) << 32 | out_h, std::memory_order_relaxed);  // tw2de (2026-10-22)
#if defined(__SWITCH__)
  {
    static const int32_t dump_at = REXCVAR_GET(tw2de_diag_dump_threads_at);
    static auto first = std::chrono::steady_clock::now();
    static int dumps = 0;
    if (dump_at > 0 && dumps < 4 &&
        std::chrono::steady_clock::now() - first >= std::chrono::seconds(dump_at + 10 * dumps)) {
      ++dumps;
      RexStallDumpNote("tw2de_diag_dump_threads_at");
    }
  }
  {
    // tw2de (2026-11-07): the Status Monitor overlay's "Viewport": the presented size (was 1280x720 always).
    static uint64_t reported = 0;
    const uint64_t size = uint64_t(out_w) << 32 | out_h;
    if (size != reported) {
      reported = size;
      RexSwitchPerfResolution(out_w, out_h);
    }
  }
#endif

  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(back, plume::RenderTextureLayout::COLOR_WRITE));
  cmd->setFramebuffer(back_fb);
  const bool scan_out = image && image->texture && image->descriptor != kInvalidDescriptor;
  // Letterbox bars (and the whole output when there is no guest image). tw2de (2026-10-19, GPU): not when the
  // frame covers the whole output, where the scan-out writes every pixel the clear would.
  bool covers = false;
  if (scan_out) {
    const FrameFit fit = FitFrame(out_w, out_h, image->source_width ? image->source_width : image->width,
                                  image->source_height ? image->source_height : image->height);
    covers = fit.viewport.x <= 0.0f && fit.viewport.y <= 0.0f && fit.viewport.x + fit.viewport.width >= float(out_w) &&
             fit.viewport.y + fit.viewport.height >= float(out_h);
  }
  if (!covers)
    cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));

  if (scan_out)
    RecordScanOut(s, *image, out_w, out_h);
  // tw2de (2026-10-23): the picture's top left on the output, for the achievement popup.
  int32_t picture_x = 0, picture_y = 0;
  if (scan_out) {
    const FrameFit fit = FitFrame(out_w, out_h, image->source_width ? image->source_width : image->width,
                                  image->source_height ? image->source_height : image->height);
    picture_x = int32_t(std::lround(fit.viewport.x));
    picture_y = int32_t(std::lround(fit.viewport.y));
  }

  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(out_w), float(out_h)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(out_w), int32_t(out_h)));
  // tw2de (2026-10-22): "Compiling shaders...", over everything (the GPU's own draw: compile_notice.h).
  const bool notice = compile_notice::DrawLocked(s, cmd, out_w, out_h);
  // tw2de (2026-10-23): "Achievement unlocked", at the picture's top left (below the notice when it is up).
  achievement_popup::DrawLocked(s, cmd, out_w, out_h, picture_x, picture_y, compile_notice::BottomLocked());

  cmd->setFramebuffer(nullptr);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  plume::RenderCommandSemaphore *present_semaphore = s.present_semaphores[image_index].get();
  detail::SubmitAndAdvance(s, acquire, present_semaphore);
  ft::Mark(timed_swap, ft::kSubmitted);
  plume::RenderCommandSemaphore *waits[] = {present_semaphore};
  bool presented;
  {
    // With vsync the present can hold until the display takes a frame.
    render_thread::WaitScope wait;
    presented = s.swap_chain->present(image_index, waits, 1);
  }
  ft::Mark(timed_swap, ft::kPresented);
  compile_notice::NotePresentedLocked(s, presented ? image_index : UINT32_MAX, notice);
  if (!presented)
    s.resize_requested.store(true, std::memory_order_release);
}

} // namespace

namespace {


int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

// tw2de (2026-10-19, latency batch 3): the fences of the last guest presents' frames, as copies (the slot's fence is
// re-armed two frames later), for the display thread (renut_engine/display_sync.cpp) to test without waiting. Its own
// mutex: the render thread holds State().mutex for whole frames.
namespace {
constexpr uint32_t kPresentFences = 8;
struct PresentFence {
  uint64_t index = 0;
#if defined(PLUME_DEKO3D)
  plume::Deko3DFenceCopy fence;
#endif
};
std::mutex g_present_fence_mutex;
PresentFence g_present_fences[kPresentFences];
std::atomic<uint64_t> g_present_count{0};
} // namespace

uint64_t Video::PresentCount() {
  return g_present_count.load(std::memory_order_acquire);
}

int Video::PresentDone(uint64_t index) {
#if defined(PLUME_DEKO3D)
  if (!index || index > g_present_count.load(std::memory_order_acquire))
    return -1;
  plume::Deko3DFenceCopy fence;
  {
    // Copied out: the test (an ioctl) runs without the mutex the render thread takes at every present.
    std::lock_guard lock(g_present_fence_mutex);
    const PresentFence &p = g_present_fences[index % kPresentFences];
    if (p.index != index)
      return 1;  // older than the ring: long finished (two frames in flight)
    fence = p.fence;
  }
  return plume::Deko3DFenceCopyDone(fence) ? 1 : 0;
#else
  (void)index;
  return -1;
#endif
}

void Video::NoteGameGamma(const uint8_t *ramp) {
  if (!ramp || !REXCVAR_GET(tw2de_renut_game_gamma))
    return;
  std::array<uint16_t, 768> r;
  for (uint32_t i = 0; i < 768; ++i)
    r[i] = uint16_t(((uint32_t(ramp[i * 2]) << 8 | ramp[i * 2 + 1]) >> 6) & 0x3FF);
  std::lock_guard lock(g_gamma.mutex);
  if (r != g_gamma.ramp || !g_gamma.version) {
    g_gamma.ramp = r;
    ++g_gamma.version;
  }
}

void Video::Present(const PresentImage *image) {
  auto &s = State();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(s.mutex);
  tw2de::frame_timing::Mark(tw2de::frame_timing::Presenting(), tw2de::frame_timing::kPresentLocked);
  if (!s.host_ready || s.shutting_down.load(std::memory_order_acquire))
    return;
  PresentLocked(s, image);
  s.last_guest_present_ms.store(NowMs(), std::memory_order_release);
  detail::LatencyTrimLocked(s);  // tw2de (2026-10-20, input latency, tw2de_deko3d_latency_trim)
  {
    // The frame just submitted: the slot before the one SubmitAndAdvance moved to.
    std::lock_guard fence_lock(g_present_fence_mutex);
    const uint64_t index = g_present_count.load(std::memory_order_relaxed) + 1;
    PresentFence &p = g_present_fences[index % kPresentFences];
    p.index = index;
#if defined(PLUME_DEKO3D)
    p.fence = plume::Deko3DCopyFence(s.fences[(s.frame + kFramesInFlight - 1) % kFramesInFlight].get());
#endif
    g_present_count.store(index, std::memory_order_release);
  }
}

// tw2de (2026-11-05, tw2de_diag_vsync_timing): waits until the GPU has finished the frame of the 'index'-th
// present; false when that is not known (too old for the ring, no fence copies, the queue failed).
bool Video::WaitPresentDone(uint64_t index) {
#if defined(PLUME_DEKO3D)
  if (!index || index > g_present_count.load(std::memory_order_acquire))
    return false;
  plume::Deko3DFenceCopy fence;
  {
    std::lock_guard lock(g_present_fence_mutex);
    const PresentFence &p = g_present_fences[index % kPresentFences];
    if (p.index != index)
      return false;
    fence = p.fence;
  }
  return plume::Deko3DFenceCopyWait(fence, State().device.get());
#else
  (void)index;
  return false;
#endif
}

bool Video::LastOutputSize(uint32_t &width, uint32_t &height) {
  const uint64_t v = g_last_output_size.load(std::memory_order_relaxed);
  width = uint32_t(v >> 32);
  height = uint32_t(v);
  return width && height;
}

bool CaptureFrame(rex::ui::RawImage &out, int timeout_ms) {
  FrameCapture &c = g_capture;
  std::unique_lock lock(c.mutex);
  const uint64_t ticket = c.requested.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (!c.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return c.served >= ticket; }) || !c.ok)
    return false;
  out = c.image;
  return true;
}

} // namespace renut::gpu

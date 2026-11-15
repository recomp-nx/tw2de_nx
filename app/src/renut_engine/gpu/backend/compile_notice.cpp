// tw2de (2026-10-22): compile_notice.h.
#include "renut_engine/gpu/backend/compile_notice.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/log.h"

#if defined(PLUME_DEKO3D) && defined(__SWITCH__)
#define RENUT_COMPILE_NOTICE 1
#include <switch.h>
#include <plume_deko3d.h>
#include "renut_engine/gpu/nx_shaders/prepass_screen.h"
#include "renut_engine/gpu/nx_shaders/ui_language.h"  // tw2de (2026-10-25)
#include "cpu/tw2de_lang_pack.h"  // tw2de (2026-10-25): a language pack's text
#include "host_shaders/notice_ps.deko3d.h"

namespace {
// tw2de (2026-10-25): the notice in the game's language (nx_shaders/ui_language.h; set by the boot menu before the
// renderer starts), rasterised once.
const char *NoticeText() {
  static constexpr const char *kText[renut::nxs::kUiLanguageCount] = {
      "Compiling shaders...", "Shader werden kompiliert...", "Compilation des shaders...", "Compilando shaders...",
      "Compilazione degli shader...",
      // TW2 (2026-11-14): Polish, Czech, Hungarian, Russian, Japanese
      "Kompilowanie shaderów...", "Kompilace shaderů...", "Shaderek fordítása...", "Компиляция шейдеров...", "シェーダーをコンパイル中...", "正在编译着色器...",
      "Gölgelendiriciler derleniyor...",  // TW2 (2026-11-15): Turkish
  };
  return tw2de::PortText("notice", kText[renut::nxs::UiLanguage()]);  // tw2de: a language pack's
}
}  // namespace
#else
#define RENUT_COMPILE_NOTICE 0
#endif

REXCVAR_DECLARE(bool, tw2de_show_compile_notice);
REXCVAR_DEFINE_BOOL(tw2de_compile_notice_test, false, "Game/Diagnostics",
                    "Switch (2026-10-23): \"Compiling shaders...\" drawn onto the image on screen as a compile does it, "
                    "without compiling: 2 s and 20 s after the first frame, each held for a second (the render thread "
                    "waits, as for a compile)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu::compile_notice {

#if RENUT_COMPILE_NOTICE
namespace {

constexpr int64_t kHoldMs = 1500;     // the notice stays up this long after the last compile started
constexpr float kFontPx = 17.0f;      // at 720 lines of output; scaled with the output's height
constexpr int32_t kMarginPx = 12;     // from the top left, at 720 lines
constexpr int kPad = 2;               // room for the shadow around the text
constexpr plume::RenderFormat kFormat = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;  // video.cpp's

struct Notice {
  bool ready = false;
  std::unique_ptr<plume::RenderShader> ps;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = kInvalidDescriptor;
  uint32_t width = 0, height = 0, margin = kMarginPx;
  // The notice's own list: the upload at start-up, then the draws onto the image on screen during a compile.
  std::unique_ptr<plume::RenderCommandList> list;
  std::unique_ptr<plume::RenderCommandFence> fence;
  bool list_pending = false;
  // The swapchain image presented last (UINT32_MAX: none yet) and whether it carries the notice.
  uint32_t last_image = UINT32_MAX;
  const plume::RenderFramebuffer *last_framebuffer = nullptr;
  bool on_last = false;
  uint64_t direct_draws = 0;
};
Notice g;
std::atomic<int64_t> g_last_compile_ms{INT64_MIN / 2};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Record(VideoState &s, plume::RenderCommandList *cmd) {
  const int32_t x = int32_t(g.margin), y = int32_t(g.margin);
  cmd->setViewports(plume::RenderViewport(float(x), float(y), float(g.width), float(g.height)));
  cmd->setScissors(plume::RenderRect(x, y, x + int32_t(g.width), y + int32_t(g.height)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(g.pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.translate[0] = -float(x);
  push.translate[1] = -float(y);
  push.texture_index = g.slot;
  push.sampler_index = kLinearClampSamplerSlot;
  push.param0 = 1.0f;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
}

// plume's compile hook: on the compiling thread, before it waits for the uam thread. Only the render thread's compiles
// hold the game (the pipeline pre-build worker's do not); it holds s.mutex there.
void DrawOnScreenLocked(VideoState &s);

void OnCompile() {
  if (!render_thread::IsCurrent() || !REXCVAR_GET(tw2de_show_compile_notice))
    return;
  g_last_compile_ms.store(NowMs(), std::memory_order_relaxed);
  DrawOnScreenLocked(State());
}

// The picture on screen again, with the notice: a frame of its own, presented.
//
// tw2de (2026-10-23): before, the notice was drawn onto the swapchain image already presented. Those images are
// colour-compressed (DkImageFlags_HwCompression) and a present stores them decompressed for the display; the notice's
// draw compressed some of their tiles again, which the display (and a screenshot) read as raw data: blocks of garbage
// in the text until the next frame. Now an image is acquired, the one on screen copied into it (the copy engine reads
// the source as it is stored), the notice drawn over it, and the image presented, which stores it decompressed.
void DrawOnScreenLocked(VideoState &s) {
  if (!g.ready || g.on_last || g.last_image == UINT32_MAX || g.last_image >= s.swap_framebuffers.size() ||
      g.last_framebuffer != s.swap_framebuffers[g.last_image].get())
    return;
  if (g.list_pending) {
    s.queue->waitForCommandFence(g.fence.get());
    g.list_pending = false;
  }
  uint32_t index = 0;
  {
    render_thread::WaitScope wait;
    if (!s.swap_chain->acquireTexture(nullptr, &index))
      return;
  }
  if (index >= s.swap_framebuffers.size())
    return;
  plume::RenderTexture *source = s.swap_chain->getTexture(g.last_image);
  plume::RenderTexture *image = s.swap_chain->getTexture(index);
  plume::RenderCommandList *cmd = g.list.get();
  cmd->begin();
  if (index != g.last_image) {
    const plume::RenderTextureBarrier copy[] = {
        plume::RenderTextureBarrier(source, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COPY_DEST)};
    cmd->barriers(plume::RenderBarrierStage::COPY, copy, 2);
    cmd->copyTexture(image, source);
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COLOR_WRITE));
  cmd->setFramebuffer(s.swap_framebuffers[index].get());
  Record(s, cmd);
  cmd->setFramebuffer(nullptr);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(image, plume::RenderTextureLayout::PRESENT));
  cmd->end();
  const plume::RenderCommandList *lists[] = {cmd};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, g.fence.get());
  g.list_pending = true;
  bool presented;
  {
    render_thread::WaitScope wait;
    presented = s.swap_chain->present(index, nullptr, 0);
  }
  if (!presented)
    return;
  g.last_image = index;
  g.last_framebuffer = s.swap_framebuffers[index].get();
  g.on_last = true;
  if (g.direct_draws++ == 0)
    RGPU_INFO("[notice] \"Compiling shaders...\" shown during a compile (a frame of its own: the picture on screen "
              "with the notice)");
}

}  // namespace

void InitLocked(VideoState &s) {
  if (g.ready || !s.device || !s.queue || !s.present_vs || !s.utility_layout)
    return;
  const uint32_t out_h = s.swap_chain && s.swap_chain->getHeight() ? s.swap_chain->getHeight() : 720;
  const float k = float(out_h) / 720.0f;
  g.margin = uint32_t(std::lround(kMarginPx * k));

  // The text, in the system's own font (as the pre-pass screen's).
  std::vector<uint8_t> coverage;
  int w = 0, h = 0;
  {
    PlFontData font{};
    const bool pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
    bool ok = false;
    if (pl && R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) &&
      (renut::nxs::UseSystemFallbackFonts(), true))
      ok = nxs::RasterLine(static_cast<const uint8_t *>(font.address), font.size, NoticeText(),
                           kFontPx * k, kPad, coverage, w, h);
    if (pl)
      plExit();
    if (!ok || w <= 0 || h <= 0) {
      RGPU_INFO("[notice] no shared font: the \"Compiling shaders...\" notice is off");
      return;
    }
  }
  g.width = uint32_t(w);
  g.height = uint32_t(h);

  g.ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(notice_ps), "notice_ps");
  if (!g.ps)
    return;
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.utility_layout.get();
  desc.vertexShader = s.present_vs.get();
  desc.pixelShader = g.ps.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = kSwapChainFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  g.pipeline = s.device->createGraphicsPipeline(desc);

  g.texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(g.width, g.height, 1, kFormat));
  g.view = g.texture ? g.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kFormat)) : nullptr;
  g.list = s.queue->createCommandList();
  g.fence = s.device->createCommandFence();
  const uint32_t row_texels = (g.width + 63) & ~63u;  // 256-byte rows
  auto staging = s.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(uint64_t(row_texels) * g.height * 4));
  if (!g.pipeline || !g.view || !g.list || !g.fence || !staging) {
    ShutdownLocked(s);
    return;
  }
  g.slot = Video::AllocateTextureSlotLocked();
  if (g.slot == kInvalidDescriptor) {
    ShutdownLocked(s);
    return;
  }
  Video::SetTextureSlotLocked(g.slot, g.texture.get(), g.view.get());

  // Uploaded once, on the notice's own list.
  auto *texels = static_cast<uint8_t *>(staging->map());
  if (!texels) {
    ShutdownLocked(s);
    return;
  }
  std::memset(texels, 0, size_t(row_texels) * g.height * 4);
  for (uint32_t y = 0; y < g.height; ++y)
    for (uint32_t x = 0; x < g.width; ++x)
      texels[(size_t(y) * row_texels + x) * 4] = coverage[size_t(y) * g.width + x];
  staging->unmap();
  plume::RenderCommandList *cmd = g.list.get();
  cmd->begin();
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(g.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(g.texture.get(), 0, 0),
                         plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), kFormat, g.width, g.height,
                                                                           1, row_texels, 0),
                         0, 0, 0, nullptr);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(g.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  cmd->end();
  const plume::RenderCommandList *lists[] = {cmd};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, g.fence.get());
  s.queue->waitForCommandFence(g.fence.get());
  staging.reset();

  plume::SetDeko3DCompileNotice(&OnCompile);
  g.ready = true;
  if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("[notice] \"Compiling shaders...\" ready: {}x{} texels in the shared font at {:.1f} px", g.width, g.height,
            kFontPx * k);
}

void ShutdownLocked(VideoState &s) {
  plume::SetDeko3DCompileNotice(nullptr);
  if (g.list_pending && s.queue && g.fence)
    s.queue->waitForCommandFence(g.fence.get());
  g.list_pending = false;
  g.ready = false;
  if (g.slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(g.slot);
  g.slot = kInvalidDescriptor;
  g.list.reset();
  g.fence.reset();
  g.view.reset();
  g.texture.reset();
  g.pipeline.reset();
  g.ps.reset();
}

bool Active() {
  return g.ready && REXCVAR_GET(tw2de_show_compile_notice) &&
         NowMs() - g_last_compile_ms.load(std::memory_order_relaxed) < kHoldMs;
}

int32_t BottomLocked() { return Active() ? int32_t(g.margin + g.height) : 0; }

bool DrawLocked(VideoState &s, plume::RenderCommandList *cmd, uint32_t out_w, uint32_t out_h) {
  if (!Active() || g.margin + g.width > out_w || g.margin + g.height > out_h)
    return false;
  Record(s, cmd);
  return true;
}

void NotePresentedLocked(VideoState &s, uint32_t image_index, bool notice_drawn) {
  g.last_image = image_index;
  g.last_framebuffer = image_index < s.swap_framebuffers.size() ? s.swap_framebuffers[image_index].get() : nullptr;
  g.on_last = notice_drawn;
  // tw2de (2026-10-23, tw2de_compile_notice_test): the on-screen draw as during a compile, then the render thread
  // held a second (as a compile holds it), so what the display shows of it can be seen.
  static int64_t first_ms = 0;
  static int done = 0;
  if (REXCVAR_GET(tw2de_compile_notice_test) && g.ready && done < 2 && image_index != UINT32_MAX) {
    const int64_t now = NowMs();
    if (!first_ms)
      first_ms = now;
    if (now - first_ms >= (done == 0 ? 2000 : 20000)) {
      ++done;
      g_last_compile_ms.store(now, std::memory_order_relaxed);
      DrawOnScreenLocked(s);
      RGPU_INFO("[notice] test {}: drawn onto the image on screen, held for a second", done);
      svcSleepThread(1000000000ll);
    }
  }
}

#else

void InitLocked(VideoState &) {}
void ShutdownLocked(VideoState &) {}
bool Active() { return false; }
int32_t BottomLocked() { return 0; }
bool DrawLocked(VideoState &, plume::RenderCommandList *, uint32_t, uint32_t) { return false; }
void NotePresentedLocked(VideoState &, uint32_t, bool) {}

#endif

}  // namespace renut::gpu::compile_notice

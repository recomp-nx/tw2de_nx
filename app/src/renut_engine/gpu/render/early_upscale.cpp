// tw2de (2026-11-10): a scene smaller than the output (1024x576 at 1280x720; 1024x576 / 1600x900 at 1920x1080)
// without FSR 3.1: its picture was finished at the scene's size, the UI drawn over it, and the whole frame scaled up at
// the scan-out (the UI soft). Now, as with FSR 3.1 before the post-processing (draw.cpp EarlyFsrBeforePostLocked), the
// scene's resolves are upscaled with Lanczos-2 into their output-size variants at the post-processing's first draw, and
// the post-processing and the UI draw at the output's size (their targets promoted). The scene's resolves are already
// single-sampled (MSAA's too), so the pass reads them, not the scene's target. Switch only (the GLSL host shaders).
#include "renut_engine/gpu/render/early_upscale.h"

#include <algorithm>
#include <memory>
#include <unordered_map>

#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/render/surfaces.h"

REXCVAR_DEFINE_INT32(tw2de_early_upscale_filter, 0, "Game/Renut",
                     "Diagnostic (2026-11-10): the early upscale's filter (tw2de_early_upscale): 0 Lanczos-2, 1 bilinear, "
                     "2 nearest texel")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(PLUME_DEKO3D)

#include "host_shaders/early_easu_ps.deko3d.h"
#include "host_shaders/early_upscale_ps.deko3d.h"
#include "host_shaders/fullscreen_vs.deko3d.h"

namespace renut::gpu {
namespace {

struct EarlyUpscale {
  bool failed = false;
  std::unique_ptr<plume::RenderShader> vs, ps, easu_ps;
  // by the target's format, | 0x80000000 for EASU
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;                         // made each run
  // TW2 (2026-11-14, EarlyUpscaleCopyLocked): the copy of the source picture
  std::unique_ptr<plume::RenderTexture> copy;
  std::unique_ptr<plume::RenderTextureView> copy_view;
  uint32_t copy_slot = kInvalidDescriptor;
  plume::RenderTextureLayout copy_layout = plume::RenderTextureLayout::UNKNOWN;
  uint32_t copy_w = 0, copy_h = 0;
  plume::RenderFormat copy_format = plume::RenderFormat::UNKNOWN;
};
EarlyUpscale g_up;

plume::RenderPipeline *PipelineFor(VideoState &s, plume::RenderFormat format, bool easu) {
  EarlyUpscale &u = g_up;
  if (!u.vs) {
    u.vs = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(fullscreen_vs), "fullscreen_vs");
    u.ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(early_upscale_ps), "early_upscale_ps");
    u.easu_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(early_easu_ps), "early_easu_ps");
    if (!u.vs || !u.ps) {
      u.failed = true;
      RGPU_WARN("[upscale] the early upscale's shaders could not be made: the frame is scaled up at the scan-out");
      return nullptr;
    }
  }
  if (easu && !u.easu_ps)
    easu = false;  // Lanczos-2 then
  auto &pipeline = u.pipelines[uint32_t(format) | (easu ? 0x80000000u : 0u)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = u.vs.get();
    desc.pixelShader = easu ? u.easu_ps.get() : u.ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline) {
      RGPU_WARN("[upscale] the early upscale's pipeline (format {}) could not be made", uint32_t(format));
      return nullptr;
    }
    pipeline->setName("renut early upscale");
  }
  return pipeline.get();
}

}  // namespace

bool EarlyUpscaleLocked(HostTexture *src, uint32_t src_w, uint32_t src_h, HostTexture *dst, uint32_t dst_w,
                        uint32_t dst_h, bool easu) {
  VideoState &s = State();
  EarlyUpscale &u = g_up;
  if (u.failed || !src || !dst || src == dst || !src->texture || !dst->texture || src->slot == UINT32_MAX ||
      !src_w || !src_h || !dst_w || !dst_h || !s.command_list)
    return false;
  src_w = std::min(src_w, src->alloc_width);
  src_h = std::min(src_h, src->alloc_height);
  dst_w = std::min(dst_w, dst->alloc_width);
  dst_h = std::min(dst_h, dst->alloc_height);
  plume::RenderPipeline *pipeline = PipelineFor(s, dst->host_format, easu);
  if (!pipeline)
    return false;
  if (u.framebuffer)
    Video::RetireLocked(std::move(u.framebuffer));
  const plume::RenderTexture *targets[1] = {dst->texture.get()};
  u.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1, nullptr));
  if (!u.framebuffer)
    return false;
  static const uint32_t kLabel = gpu_profile::Label("early upscale");
  plume::RenderCommandList *cmd = s.command_list;
  gpu_profile::Mark(s, kLabel);
  cmd->setFramebuffer(nullptr);
  TransitionTextureLocked(src, plume::RenderTextureLayout::SHADER_READ);
  TransitionTextureLocked(dst, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  // scale: the picture's last texel centre in texture coordinates (the fetches' clamp); translate: its size in texels.
  push.scale[0] = (float(src_w) - 0.5f) / float(src->alloc_width);
  push.scale[1] = (float(src_h) - 0.5f) / float(src->alloc_height);
  push.translate[0] = float(src_w);
  push.translate[1] = float(src_h);
  push.texture_index = src->slot;
  push.sampler_index = kLinearClampSamplerSlot;
  static const float filter = float(REXCVAR_GET(tw2de_early_upscale_filter));
  push.param0 = filter;
  cmd->setFramebuffer(u.framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dst_w), float(dst_h)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(dst_w), int32_t(dst_h)));
  cmd->setPipeline(pipeline);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  cmd->setFramebuffer(nullptr);
  TransitionTextureLocked(dst, plume::RenderTextureLayout::SHADER_READ);
  static uint32_t logged = 0;
  if (logged++ < 4)
    RGPU_INFO("[upscale] the scene's resolve upscaled before the post-processing ({}): {}x{} -> {}x{}",
              easu ? "FSR 1 EASU" : "Lanczos-2", src_w, src_h, dst_w, dst_h);
  return true;
}

// tw2de (2026-11-11): the same draw from a render target into another (early_upscale.h EarlyUpscaleSurfaceLocked).
bool EarlyUpscaleSurfaceLocked(HostSurface *src, HostSurface *dst) {
  VideoState &s = State();
  EarlyUpscale &u = g_up;
  if (u.failed || !src || !dst || src == dst || !src->texture || !dst->texture || src->depth || dst->depth ||
      !s.command_list)
    return false;
  const uint32_t src_w = src->scale.Size(src->width), src_h = src->scale.Size(src->height);
  const uint32_t dst_w = dst->scale.Size(dst->width), dst_h = dst->scale.Size(dst->height);
  if (!src_w || !src_h || !dst_w || !dst_h)
    return false;
  const uint32_t slot = SurfaceSampleSlotLocked(src);
  if (slot == UINT32_MAX || slot == kInvalidDescriptor)
    return false;
  plume::RenderPipeline *pipeline = PipelineFor(s, dst->format, false);
  if (!pipeline)
    return false;
  if (u.framebuffer)
    Video::RetireLocked(std::move(u.framebuffer));
  const plume::RenderTexture *targets[1] = {dst->texture.get()};
  u.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1, nullptr));
  if (!u.framebuffer)
    return false;
  plume::RenderCommandList *cmd = s.command_list;
  cmd->setFramebuffer(nullptr);
  TransitionSurfaceLocked(src, plume::RenderTextureLayout::SHADER_READ);
  TransitionSurfaceLocked(dst, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = (float(src_w) - 0.5f) / float(src_w);
  push.scale[1] = (float(src_h) - 0.5f) / float(src_h);
  push.translate[0] = float(src_w);
  push.translate[1] = float(src_h);
  push.texture_index = slot;
  push.sampler_index = kLinearClampSamplerSlot;
  static const float filter = float(REXCVAR_GET(tw2de_early_upscale_filter));
  push.param0 = filter;
  cmd->setFramebuffer(u.framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dst_w), float(dst_h)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(dst_w), int32_t(dst_h)));
  cmd->setPipeline(pipeline);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  cmd->setFramebuffer(nullptr);
  static uint32_t logged = 0;
  if (logged++ < 4)
    RGPU_INFO("[upscale] a render target's picture upscaled into its output-size variant (the GUI's): {}x{} -> {}x{}",
              src_w, src_h, dst_w, dst_h);
  return true;
}

// TW2 (2026-11-14): early_upscale.h EarlyUpscaleCopyLocked.
bool EarlyUpscaleCopyLocked(HostSurface *src, uint32_t src_w, uint32_t src_h, HostSurface *dst, bool easu) {
  VideoState &s = State();
  EarlyUpscale &u = g_up;
  if (u.failed || !src || !dst || !src->texture || !dst->texture || src->depth || dst->depth || !s.command_list ||
      src->format != dst->format || src->samples != plume::RenderSampleCount::COUNT_1 ||
      dst->samples != plume::RenderSampleCount::COUNT_1)
    return false;
  src_w = std::min(src_w, src->scale.Size(src->width));
  src_h = std::min(src_h, src->scale.Size(src->height));
  const uint32_t dst_w = dst->scale.Size(dst->width), dst_h = dst->scale.Size(dst->height);
  if (!src_w || !src_h || !dst_w || !dst_h)
    return false;
  plume::RenderPipeline *pipeline = PipelineFor(s, dst->format, easu);
  if (!pipeline)
    return false;
  if (u.copy_w != src_w || u.copy_h != src_h || u.copy_format != src->format || !u.copy) {
    if (u.copy_slot != kInvalidDescriptor)
      Video::FreeTextureSlotLocked(u.copy_slot);
    u.copy_slot = kInvalidDescriptor;
    if (u.copy_view)
      Video::RetireLocked(std::move(u.copy_view));
    if (u.copy)
      Video::RetireLocked(std::move(u.copy));
    u.copy = s.device->createTexture(plume::RenderTextureDesc::ColorTarget(src_w, src_h, src->format));
    if (!u.copy)
      return false;
    u.copy->setName("renut final upscale source");
    u.copy_view = u.copy->createTextureView(plume::RenderTextureViewDesc::Texture2D(src->format));
    u.copy_slot = u.copy_view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
    if (u.copy_slot == kInvalidDescriptor)
      return false;
    Video::SetTextureSlotLocked(u.copy_slot, u.copy.get(), u.copy_view.get());
    u.copy_layout = plume::RenderTextureLayout::UNKNOWN;
    u.copy_w = src_w;
    u.copy_h = src_h;
    u.copy_format = src->format;
  }
  if (u.framebuffer)
    Video::RetireLocked(std::move(u.framebuffer));
  const plume::RenderTexture *targets[1] = {dst->texture.get()};
  u.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1, nullptr));
  if (!u.framebuffer)
    return false;
  static const uint32_t kLabel = gpu_profile::Label("final upscale");
  plume::RenderCommandList *cmd = s.command_list;
  gpu_profile::Mark(s, kLabel);
  // The picture copied (the pass may write the surface it comes from).
  cmd->setFramebuffer(nullptr);
  {
    plume::RenderTextureBarrier b[2];
    uint32_t n = 0;
    if (src->layout != plume::RenderTextureLayout::COPY_SOURCE) {
      b[n++] = plume::RenderTextureBarrier(src->texture.get(), plume::RenderTextureLayout::COPY_SOURCE);
      src->layout = plume::RenderTextureLayout::COPY_SOURCE;
    }
    if (u.copy_layout != plume::RenderTextureLayout::COPY_DEST) {
      b[n++] = plume::RenderTextureBarrier(u.copy.get(), plume::RenderTextureLayout::COPY_DEST);
      u.copy_layout = plume::RenderTextureLayout::COPY_DEST;
    }
    if (n)
      cmd->barriers(plume::RenderBarrierStage::COPY, nullptr, 0, b, n);
  }
  const plume::RenderBox box(0, 0, int32_t(src_w), int32_t(src_h));
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(u.copy.get()),
                         plume::RenderTextureCopyLocation::Subresource(src->texture.get()), 0, 0, 0, &box);
  {
    plume::RenderTextureBarrier b(u.copy.get(), plume::RenderTextureLayout::SHADER_READ);
    u.copy_layout = plume::RenderTextureLayout::SHADER_READ;
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, &b, 1);
  }
  TransitionSurfaceLocked(dst, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = (float(src_w) - 0.5f) / float(src_w);
  push.scale[1] = (float(src_h) - 0.5f) / float(src_h);
  push.translate[0] = float(src_w);
  push.translate[1] = float(src_h);
  push.texture_index = u.copy_slot;
  push.sampler_index = kLinearClampSamplerSlot;
  static const float filter = float(REXCVAR_GET(tw2de_early_upscale_filter));
  push.param0 = filter;
  cmd->setFramebuffer(u.framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dst_w), float(dst_h)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(dst_w), int32_t(dst_h)));
  cmd->setPipeline(pipeline);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  cmd->setFramebuffer(nullptr);
  static uint32_t logged = 0;
  if (logged++ < 4)
    RGPU_INFO("[upscale] the game's last pass drawn at the scene's size, then enlarged ({}): {}x{} -> {}x{}",
              easu ? "FSR 1 EASU" : "Lanczos-2", src_w, src_h, dst_w, dst_h);
  return true;
}

}  // namespace renut::gpu

#else  // !PLUME_DEKO3D

namespace renut::gpu {
bool EarlyUpscaleLocked(HostTexture *, uint32_t, uint32_t, HostTexture *, uint32_t, uint32_t, bool) { return false; }
bool EarlyUpscaleSurfaceLocked(HostSurface *, HostSurface *) { return false; }
bool EarlyUpscaleCopyLocked(HostSurface *, uint32_t, uint32_t, HostSurface *, bool) { return false; }
}  // namespace renut::gpu

#endif

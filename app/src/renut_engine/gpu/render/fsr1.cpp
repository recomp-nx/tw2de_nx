// tw2de (2026-10-25, tw2de_renut_upscale_filter 3, the boot menu's "Upscaler: FSR1"): FSR 1's second pass, RCAS.
//
// AMD FidelityFX Super Resolution 1.0 (MIT) is two passes: EASU upscales (shaders/deko3d/fsr1_easu_ps.glsl, drawn as the
// pixel shader of the game's upscale draw: render/draw.cpp), RCAS sharpens the upscaled picture
// (shaders/deko3d/fsr1_rcas_ps.glsl). RCAS runs right after the upscale draw, on its 1280x720 target, before the game
// draws its UI over it: the picture is copied into a texture of ours, and RCAS reads the copy and writes the target
// (red, green and blue; the target keeps its alpha). Once a frame. Switch only (the GLSL host shaders).
#include "renut_engine/gpu/render/fsr1.h"

#include <cmath>
#include <memory>
#include <unordered_map>

#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"

REXCVAR_DEFINE_DOUBLE(tw2de_fsr1_sharpness, 0.0, "Game/Enhancements",
                      "FSR 1's RCAS sharpening (tw2de_renut_upscale_filter 3): its attenuation in stops, 0 = the "
                      "sharpest and the default here, 2 = little (AMD's default is 0.2)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(PLUME_DEKO3D)

#include "host_shaders/fsr1_rcas_ps.deko3d.h"
#include "host_shaders/fullscreen_vs.deko3d.h"

namespace renut::gpu {
namespace {

struct Rcas {
  bool failed = false;
  std::unique_ptr<plume::RenderShader> vs, ps;
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines;  // by the target's format
  std::unique_ptr<plume::RenderTexture> src;
  std::unique_ptr<plume::RenderTextureView> src_view;
  uint32_t src_slot = kInvalidDescriptor;
  plume::RenderTextureLayout src_layout = plume::RenderTextureLayout::UNKNOWN;
  uint32_t width = 0, height = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;  // made each run
  uint64_t last_frame = UINT64_MAX;
};
Rcas g_rcas;

bool Fail(const char *what) {
  g_rcas.failed = true;
  RGPU_WARN("[fsr1] RCAS: {} could not be made: FSR 1 runs without its sharpening", what);
  return false;
}

bool Prepare(VideoState &s, plume::RenderFormat format, uint32_t width, uint32_t height) {
  Rcas &r = g_rcas;
  if (!r.vs) {
    r.vs = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(fullscreen_vs), "fullscreen_vs");
    r.ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(fsr1_rcas_ps), "fsr1_rcas_ps");
    if (!r.vs || !r.ps)
      return Fail("its shaders");
  }
  if (r.width != width || r.height != height || r.format != format || !r.src) {
    if (r.src_slot != kInvalidDescriptor)
      Video::FreeTextureSlotLocked(r.src_slot);
    r.src_slot = kInvalidDescriptor;
    if (r.src_view)
      Video::RetireLocked(std::move(r.src_view));
    if (r.src)
      Video::RetireLocked(std::move(r.src));
    r.src = s.device->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, format));
    if (!r.src)
      return Fail("its texture");
    r.src->setName("renut FSR1 RCAS source");
    r.src_view = r.src->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
    r.src_slot = r.src_view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
    if (r.src_slot == kInvalidDescriptor)
      return Fail("its texture's view");
    Video::SetTextureSlotLocked(r.src_slot, r.src.get(), r.src_view.get());
    r.src_layout = plume::RenderTextureLayout::UNKNOWN;
    r.width = width;
    r.height = height;
    r.format = format;
  }
  auto &pipeline = r.pipelines[uint32_t(format)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = r.vs.get();
    desc.pixelShader = r.ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.renderTargetBlend[0].renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::RED) |
                                                      uint8_t(plume::RenderColorWriteEnable::GREEN) |
                                                      uint8_t(plume::RenderColorWriteEnable::BLUE);
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return Fail("its pipeline");
    pipeline->setName("renut FSR1 RCAS");
  }
  return true;
}

}  // namespace

bool Fsr1RcasRunLocked(HostSurface *surface) {
  VideoState &s = State();
  Rcas &r = g_rcas;
  if (r.failed || !surface || surface->depth || !surface->texture ||
      surface->samples != plume::RenderSampleCount::COUNT_1 ||
      (surface->format != plume::RenderFormat::R8G8B8A8_UNORM &&
       surface->format != plume::RenderFormat::B8G8R8A8_UNORM))
    return false;
  const uint64_t frame = TextureFrameLocked();
  if (r.last_frame == frame)
    return false;
  const uint32_t width = uint32_t(surface->scale.Coord(int32_t(surface->width)));
  const uint32_t height = uint32_t(surface->scale.Coord(int32_t(surface->height)));
  if (!width || !height || !s.command_list || !Prepare(s, surface->format, width, height))
    return false;
  if (r.framebuffer)
    Video::RetireLocked(std::move(r.framebuffer));
  const plume::RenderTexture *targets[1] = {surface->texture.get()};
  r.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1, nullptr));
  if (!r.framebuffer)
    return false;
  r.last_frame = frame;
  static const uint32_t kLabel = gpu_profile::Label("FSR1 RCAS");
  static const float sharpness = float(std::exp2(-REXCVAR_GET(tw2de_fsr1_sharpness)));
  plume::RenderCommandList *cmd = s.command_list;
  gpu_profile::Mark(s, kLabel);

  // The picture copied (the pass reads it while it writes the target).
  cmd->setFramebuffer(nullptr);
  {
    plume::RenderTextureBarrier b[2];
    uint32_t n = 0;
    if (surface->layout != plume::RenderTextureLayout::COPY_SOURCE) {
      b[n++] = plume::RenderTextureBarrier(surface->texture.get(), plume::RenderTextureLayout::COPY_SOURCE);
      surface->layout = plume::RenderTextureLayout::COPY_SOURCE;
    }
    if (r.src_layout != plume::RenderTextureLayout::COPY_DEST) {
      b[n++] = plume::RenderTextureBarrier(r.src.get(), plume::RenderTextureLayout::COPY_DEST);
      r.src_layout = plume::RenderTextureLayout::COPY_DEST;
    }
    if (n)
      cmd->barriers(plume::RenderBarrierStage::COPY, nullptr, 0, b, n);
  }
  const plume::RenderBox box(0, 0, int32_t(width), int32_t(height));
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(r.src.get()),
                         plume::RenderTextureCopyLocation::Subresource(surface->texture.get()), 0, 0, 0, &box);
  {
    plume::RenderTextureBarrier b[2] = {
        plume::RenderTextureBarrier(r.src.get(), plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(surface->texture.get(), plume::RenderTextureLayout::COLOR_WRITE)};
    r.src_layout = plume::RenderTextureLayout::SHADER_READ;
    surface->layout = plume::RenderTextureLayout::COLOR_WRITE;
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, b, 2);
  }

  // RCAS into the target.
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = 1.0f / float(width);
  push.scale[1] = 1.0f / float(height);
  push.translate[0] = float(width);
  push.translate[1] = float(height);
  push.texture_index = r.src_slot;
  push.sampler_index = kPointClampSamplerSlot;
  push.param0 = sharpness;
  cmd->setFramebuffer(r.framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  cmd->setPipeline(g_rcas.pipelines[uint32_t(surface->format)].get());
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  ++surface->writes;  // tw2de_renut_resolve_skip: the target changed
  static bool logged = false;
  if (!logged) {
    logged = true;
    RGPU_INFO("[fsr1] RCAS on the upscale's {}x{} target, sharpness {:.3f}", width, height, sharpness);
  }
  return true;
}

}  // namespace renut::gpu

#else  // !PLUME_DEKO3D

namespace renut::gpu {
bool Fsr1RcasRunLocked(HostSurface *) {
  return false;
}
}  // namespace renut::gpu

#endif

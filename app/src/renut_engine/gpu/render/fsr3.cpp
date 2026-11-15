// tw2de (2026-11-06, tw2de_fsr3): AMD FidelityFX Super Resolution 3.1's upscaler on deko3d (render/fsr3.h).
//
// The passes are the FidelityFX SDK 1.1.4 shaders as the xc1fsr port has them (tools/fsr3/convert_fsr3.py makes
// shaders/deko3d/fsr3_<pass>_cs.glsl and render/fsr3_bindings.h from them), driven the way the SDK's
// ffx_fsr3upscaler.cpp and the port's fsr.cpp drive them: prepare inputs (dilated depth and motion, the farthest
// depth's mip 1, the luma), the shading change pyramid (SPD) and shading change, prepare reactivity, luma instability,
// accumulate (into the upscaled output; no sharpening pass). Its own textures ping-pong by frame (accumulation, luma,
// luma history, the upscaled history).
//
// Where: the game's upscale draw (its post-processed picture to the 1280x720 target, before the UI), whose picture is
// FSR's colour input; the scene's depth target and the motion vector target (render/draw.cpp) the other inputs. The
// result is copied into the draw's target, and the draw is not made. Every resource FSR reads is single-sampled: with
// MSAA on the upscale draw is the game's own.
//
// Bindings: plume's pipeline layouts have no storage images, so the passes are dispatched with deko3d's own bindings
// (plume::Deko3DDispatchRaw): textures as handles of the renderer's bindless descriptor set (image id | sampler id
// << 20), images as ids of load/store descriptors written into the same set (Deko3DWriteStorageDescriptor), uniform
// buffers from the upload ring.
#include "renut_engine/gpu/render/fsr3.h"
#include "renut_engine/gpu/render/dynamic_resolution.h"  // tw2de (2026-11-07)

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/backend/gpu_profile.h"  // 2026-11-07: each pass its own line in renut_gpu_profile
#include "renut_engine/gpu/tw2de_renut_tuning.h"  // NextToExecutable
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/render/upload_ring.h"

REXCVAR_DEFINE_INT32(tw2de_fsr3, 0, "Game/Enhancements",
                     "tw2de (2026-11-06): AMD FSR 3.1 upscaling in place of the game's upscale (its picture to the "
                     "1280x720 output), with the scene's per-object motion vectors (tw2de_motion_vectors on) and "
                     "the scene jittered; needs anti-aliasing off. 0 off, 1 on")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tw2de (2026-11-07): FSR's sharpening (RCAS) after it, as xc1fsr sets it (its fsr_sharpening / fsr_sharpness:
// CONFIG.md, source/program/fsr.cpp; adaptive its default).
REXCVAR_DEFINE_INT32(tw2de_fsr3_sharpening, 2, "Game/Enhancements",
                     "FSR 3.1's sharpening (RCAS) after upscaling: 0 off, 1 fixed (tw2de_fsr3_sharpness), 2 adaptive "
                     "(xc1fsr's: extra strong, -0.25, but held back on thin horizontal line structures such as "
                     "reflections on water, which fixed sharpening turns into stripes)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(tw2de_fsr3_sharpness, -0.25, "Game/Enhancements",
                      "The fixed sharpening's strength (tw2de_fsr3_sharpening 1) in stops: -0.25 extra strong, 0 "
                      "AMD's strongest, up to 0.5 weaker")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(tw2de_fsr3_diag, false, "Game/Diagnostics",
                    "tw2de_fsr3 diagnostic (2026-11-06): the file fsr3_view next to the executable (a number, read "
                    "once a second) picks one of FSR's textures to show instead of its output: 1 the colour input, 2 the "
                    "motion input, 3 the depth input, 4 dilated motion, 5 dilated depth, 6 dilated reactive masks, 7 "
                    "luma instability, 8 new locks, 9 the upscaled history written this frame, 10 the current luma, 11 the "
                    "accumulation written this frame, 12 the farthest depth's mip 1; 0 FSR's output");

REXCVAR_DEFINE_BOOL(tw2de_diag_fsr3_no_jitter, false, "Game/Diagnostics",
                    "tw2de_fsr3 test (2026-11-07, hitches): the scene drawn without FSR's sub-pixel jitter (FSR told "
                    "so); for telling the jitter's part in a problem from the rest")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(tw2de_diag_fsr3_skip_passes, false, "Game/Diagnostics",
                    "tw2de_fsr3 test (2026-11-07, hitches): FSR's passes not run: its input picture copied to the "
                    "output as it is (view 1 of tw2de_fsr3_diag), the rest of FSR's frame (motion vectors, MSAA off, "
                    "jitter unless tw2de_diag_fsr3_no_jitter) unchanged: FSR's GPU work out of a problem")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tw2de (2026-11-07): the Full barrier (the GPU idle after every pass) gave the whole picture hitches at irregular
// times with FSR 3.1 on (seen rotating the camera, also at GPU 1075 MHz); the Primitives one with the texture, image
// and shader caches dropped (as xc1fsr's NVN barrier) has none and the same picture and pass times: the default.
REXCVAR_DEFINE_BOOL(tw2de_diag_fsr3_full_barriers, false, "Game/Diagnostics",
                    "tw2de_fsr3 test: deko3d's Full barrier between FSR's passes (the GPU idle after every pass; "
                    "hitched) instead of the Primitives one with the texture, image and shader caches dropped")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(tw2de_diag_fsr3_frames, false, "Game/Diagnostics",
                    "tw2de_fsr3 diagnostic (2026-11-07, hitches): every FSR run that looks wrong for its frame "
                    "(frame starts since the last run other than 1, scene draws without last frame's constants, a "
                    "history reset, a scene drawn unjittered or with another jitter than FSR's, a long gap) as an "
                    "[fsr3 frames] line (at most 40 in 10 s), and a summary every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(PLUME_DEKO3D)

#include <plume_deko3d.h>

#include "host_shaders/fsr3_accumulate_cs.deko3d.h"
#include "host_shaders/fsr3_accumulate_li_bil_cs.deko3d.h"
#include "host_shaders/fsr3_accumulate_sharp_cs.deko3d.h"         // tw2de (2026-11-07): sharpening
#include "host_shaders/fsr3_accumulate_li_bil_sharp_cs.deko3d.h"
#include "host_shaders/fsr3_rcas_cs.deko3d.h"
#include "host_shaders/fsr3_rcas_adapt_cs.deko3d.h"
#include "host_shaders/fsr3_clear_cs.deko3d.h"
#include "host_shaders/fsr3_clear_uint_cs.deko3d.h"
#include "host_shaders/fsr3_clear_buffer_cs.deko3d.h"
#include "host_shaders/fsr3_luma_instability_cs.deko3d.h"
#include "host_shaders/fsr3_prepare_inputs_cs.deko3d.h"
#include "host_shaders/fsr3_prepare_reactivity_cs.deko3d.h"
#include "host_shaders/fsr3_shading_change_cs.deko3d.h"
#include "host_shaders/fsr3_shading_change_pyramid_cs.deko3d.h"
#include "host_shaders/fsr3_view_cs.deko3d.h"
#include "renut_engine/gpu/render/fsr3_bindings.h"

namespace renut::gpu {
namespace {

using fsr3::Fsr3Bind;
using fsr3::Fsr3BindKind;

// FSR's own textures (the port's fsr.cpp, Describe): render size w x h, half size, upscale size.
enum Res {
  R_ACC1, R_ACC2, R_LUMA1, R_LUMA2, R_FP16X1, R_SHADING_CHANGE, R_NEW_LOCKS, R_UPSCALED1, R_UPSCALED2, R_SPD_MIPS,
  R_FARTHEST_MIP1, R_LUMA_HIST1, R_LUMA_HIST2, R_DILATED_REACTIVE, R_DILATED_MV, R_DILATED_DEPTH, R_RECON_DEPTH,
  R_OUTPUT, R_COUNT
};
constexpr uint32_t kMaxLevels = 12;

struct Resource {
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint32_t width = 0, height = 0, levels = 1;
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;                     // all levels, sampled
  std::unique_ptr<plume::RenderTextureView> level_views[kMaxLevels];  // one level each, for stores
  uint32_t texture_slot = kInvalidDescriptor, texture_id = ~0u;       // sampled
  uint32_t image_slots[kMaxLevels], image_ids[kMaxLevels];            // load/store, per level
  Resource() {
    std::fill(std::begin(image_slots), std::end(image_slots), kInvalidDescriptor);
    std::fill(std::begin(image_ids), std::end(image_ids), ~0u);
  }
};

struct Pass {
  const char *name;
  const Fsr3Bind *binds;
  uint32_t count;
  std::unique_ptr<plume::RenderShader> shader;
  std::unique_ptr<plume::RenderPipeline> pipeline;
};

// P_ACCUMULATE: upscaling (render size < output size); P_ACCUMULATE_NATIVE: render size == output size (xc1fsr's
// accumulate_li_bil: luma instability merged in, so no P_LUMA_INSTABILITY; bilinear history; constant sample weights).
enum PassId { P_PREPARE_INPUTS, P_SPD, P_SHADING_CHANGE, P_PREPARE_REACTIVITY, P_LUMA_INSTABILITY, P_ACCUMULATE,
              P_ACCUMULATE_NATIVE, P_CLEAR, P_CLEAR_UINT, P_CLEAR_BUFFER, P_VIEW,
              // tw2de (2026-11-07): sharpening (tw2de_fsr3_sharpening): the accumulates without the output, RCAS
              P_ACCUMULATE_SHARP, P_ACCUMULATE_NATIVE_SHARP, P_RCAS, P_RCAS_ADAPT, P_COUNT };

// The constant buffers as the shaders lay them out (std140; fsr3_*_cs.glsl).
struct alignas(16) FsrConstants {
  int32_t iRenderSize[2], iPreviousFrameRenderSize[2], iUpscaleSize[2], iPreviousFrameUpscaleSize[2], iMaxRenderSize[2],
      iMaxUpscaleSize[2];
  float fDeviceToViewDepth[4];
  float fJitter[2], fPreviousFrameJitter[2], fMotionVectorScale[2], fDownscaleFactor[2],
      fMotionVectorJitterCancellation[2];
  float fTanHalfFOV, fJitterSequenceLength, fDeltaTime, fDeltaPreExposure, fViewSpaceToMetersFactor, fFrameIndex,
      fVelocityFactor, fReactivenessScale, fShadingChangeScale, fAccumulationAddedPerFrame, fMinDisocclusionAccumulation;
};
static_assert(offsetof(FsrConstants, fDeviceToViewDepth) == 48 && offsetof(FsrConstants, fTanHalfFOV) == 104);
struct SpdConstants {
  uint32_t mips, numWorkGroups, workGroupOffset[2], renderSize[2];
};
struct NvnDims {
  int32_t spdMipDims[3][4];
  int32_t maskDims[4];
  int32_t farthestDims[4];
  float mvDecode[4];
  float mvEncoding[4];
  int32_t depthDims[4];
  float nativeOffSq[12];
  float nativeBoxW[12];
  float nativeLanczosW[12];
  float u[24];
  float xc2Tmaa[12];
};
struct ClearConstants {
  uint32_t value[4];
  uint32_t size[4];
};

struct State3 {
  bool failed = false, logged = false;
  Pass passes[P_COUNT];
  Resource res[R_COUNT];
  uint32_t render_w = 0, render_h = 0, up_w = 0, up_h = 0;
  plume::RenderFormat out_format = plume::RenderFormat::UNKNOWN;
  // Inputs' sampled slots (rewritten when the texture changes).
  uint32_t motion_slot = kInvalidDescriptor;
  // The reconstructed previous depth (render width x height uints; tools/fsr3/convert_fsr3.py: a storage buffer, as
  // image atomics stopped the pass).
  std::unique_ptr<plume::RenderBuffer> recon;
  uint64_t recon_address = 0;
  uint32_t recon_words = 0;
  const plume::RenderTexture *motion_in = nullptr;
  uint32_t linear_sampler = 0, point_sampler = 0;
  // The sequence.
  uint64_t frames = 0;  // FSR runs
  bool first = true;
  bool last_reset = false;  // tw2de (2026-11-08): the last run started the history again
  float jitter[2] = {}, prev_jitter[2] = {};
  float jitter_ndc[2] = {};
  uint32_t phase = 0, phase_count = 8;
  int32_t prev_render[2] = {}, prev_up[2] = {};
  float frame_index = 0;
  bool odd = false;
  std::chrono::steady_clock::time_point last_run{};
  // The jitter only while FSR runs: set at a frame's start when FSR ran in the frame before (a frame without it, such as
  // the title screen or a frame at another size, would show the scene shaking).
  bool ran_this_frame = false, jitter_on = false;
  int view = 0;  // tw2de_fsr3_diag
  float view_scale = 0.0f, view_bias = 0.0f;  // 0 scale: the view's own
  // tw2de (2026-11-08, the 1600x900 + dynamic resolution shimmer): the view file's fourth number, test switches
  // (Fsr3DiagFlags): 1 no jitter, 2 the per-tile colour restores skipped, 4 the per-tile depth restores skipped, 8 no
  // motion vectors, 16 a full barrier before each per-tile colour restore, 32 FSR's history reset every frame.
  uint32_t diag_flags = 0;
  bool output_ready = false;           // tw2de (2026-11-08): Fsr3CopyOutputLocked may copy from the output
  bool output_layout_general = true;   // the output's layout: GENERAL (the passes') or COPY_SOURCE
  std::chrono::steady_clock::time_point view_read{};
};
State3 g3;

// tw2de_diag_fsr3_frames (2026-11-07): what each FSR run got against what it should get (see the cvar).
void FramesDiag(const Fsr3SceneInputs &scene, bool reset) {
  static const bool on = REXCVAR_GET(tw2de_diag_fsr3_frames);
  if (!on)
    return;
  struct S {
    uint64_t last_frame = 0, runs = 0, odd = 0, gap_frames = 0, unmatched_runs = 0, resets = 0, unjittered = 0,
             mismatched = 0, lines = 0, slow = 0;
    double worst_ms = 0;
    std::chrono::steady_clock::time_point last{}, report{};
  };
  static S s;
  const auto now = std::chrono::steady_clock::now();
  const double ms = s.last == std::chrono::steady_clock::time_point{}
                        ? 0.0
                        : std::chrono::duration<double, std::milli>(now - s.last).count();
  const uint64_t frames = s.last_frame ? scene.frame - s.last_frame : 1;
  const float jx = g3.jitter_ndc[0], jy = g3.jitter_ndc[1];
  const bool unjittered = scene.jittered == 0;
  const bool mismatch = !unjittered && (scene.drawn_jitter[0] != jx || scene.drawn_jitter[1] != jy || scene.jitter_mixed);
  const bool slow = ms > 40.0;
  const bool odd = frames != 1 || scene.unmatched || reset || unjittered || mismatch || slow;
  ++s.runs;
  s.gap_frames += frames != 1;
  s.unmatched_runs += scene.unmatched != 0;
  s.resets += reset;
  s.unjittered += unjittered;
  s.mismatched += mismatch;
  s.slow += slow;
  s.worst_ms = std::max(s.worst_ms, ms);
  if (odd) {
    ++s.odd;
    if (s.lines++ < 40)
      RGPU_INFO("[fsr3 frames] run {}: frame {} ({} frame starts since the last run), {:.1f} ms since it; scene draws "
                "{} matched {} unmatched{}; jitter: FSR ({:.6f}, {:.6f}) drawn ({:.6f}, {:.6f}) in {} draws{}{}",
                s.runs, scene.frame, frames, ms, scene.matched, scene.unmatched, reset ? ", history RESET" : "", jx,
                jy, scene.drawn_jitter[0], scene.drawn_jitter[1], scene.jittered,
                scene.jitter_mixed ? " (draws differ)" : "", mismatch ? " MISMATCH" : "");
  }
  if (now - s.report >= std::chrono::seconds(10)) {
    if (s.report != std::chrono::steady_clock::time_point{})
      RGPU_INFO("[fsr3 frames] 10 s: {} runs, {} odd: {} with frame starts other than 1, {} with unmatched scene draws, "
                "{} resets, {} unjittered scenes, {} jitter mismatches, {} gaps over 40 ms (worst {:.1f} ms)",
                s.runs, s.odd, s.gap_frames, s.unmatched_runs, s.resets, s.unjittered, s.mismatched, s.slow,
                s.worst_ms);
    s = S{};
    s.report = now;
  }
  s.last = now;
  s.last_frame = scene.frame;
}

bool Fail(const char *what) {
  g3.failed = true;
  RGPU_WARN("[fsr3] {} could not be made: no FSR 3 (the game's upscale instead)", what);
  return false;
}

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (uint32_t i = index; i > 0; i /= base) {
    f /= float(base);
    r += f * float(i % base);
  }
  return r;
}

uint32_t Levels(uint32_t w, uint32_t h) {
  uint32_t l = 1;
  while ((w > 1 || h > 1) && l < kMaxLevels) {
    w = std::max(w / 2, 1u);
    h = std::max(h / 2, 1u);
    ++l;
  }
  return l;
}

void ReleaseResource(Resource &r) {
  if (r.texture_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(r.texture_slot);
  for (uint32_t &slot : r.image_slots)
    if (slot != kInvalidDescriptor)
      Video::FreeTextureSlotLocked(slot);
  for (auto &v : r.level_views)
    if (v)
      Video::RetireLocked(std::move(v));
  if (r.view)
    Video::RetireLocked(std::move(r.view));
  if (r.texture)
    Video::RetireLocked(std::move(r.texture));
  r = Resource();
}

bool MakeResource(VideoState &s, Resource &r, plume::RenderFormat format, uint32_t w, uint32_t h, uint32_t levels) {
  r.format = format;
  r.width = std::max(w, 1u);
  r.height = std::max(h, 1u);
  r.levels = std::min(levels, kMaxLevels);
  r.texture = s.device->createTexture(
      plume::RenderTextureDesc::Texture2D(r.width, r.height, r.levels, format, plume::RenderTextureFlag::STORAGE));
  if (!r.texture)
    return false;
  r.texture->setName("tw2de FSR 3");
  r.view = r.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
  r.texture_slot = r.view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
  if (r.texture_slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(r.texture_slot, r.texture.get(), r.view.get());
  r.texture_id = plume::Deko3DImageId(s.texture_set.get(), r.texture_slot);
  for (uint32_t level = 0; level < r.levels; ++level) {
    plume::RenderTextureViewDesc d = plume::RenderTextureViewDesc::Texture2D(format);
    d.mipSlice = level;
    d.mipLevels = 1;
    r.level_views[level] = r.texture->createTextureView(d);
    r.image_slots[level] = r.level_views[level] ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
    if (r.image_slots[level] == kInvalidDescriptor)
      return false;
    r.image_ids[level] = plume::Deko3DWriteStorageDescriptor(s.texture_set.get(), r.image_slots[level], r.texture.get(),
                                                             r.level_views[level].get());
    if (r.image_ids[level] == ~0u)
      return false;
  }
  return r.texture_id != ~0u;
}

bool PreparePasses(VideoState &s) {
  if (g3.passes[0].pipeline)
    return true;
  struct Def {
    PassId id;
    const char *name;
    HostShaderBlob blob;
    const Fsr3Bind *binds;
    uint32_t count;
  };
  static const Fsr3Bind kClearBinds[] = {{"Fsr3Clear", Fsr3BindKind::kUbo, 0}, {"g_Image", Fsr3BindKind::kImage, 0}};
  static const Fsr3Bind kClearBufferBinds[] = {{"Fsr3Clear", Fsr3BindKind::kUbo, 0},
                                               {"Fsr3ReconDepth_t", Fsr3BindKind::kSsbo, 0}};
  static const Fsr3Bind kViewBinds[] = {{"Fsr3View", Fsr3BindKind::kUbo, 0},
                                        {"g_Texture", Fsr3BindKind::kTexture, 0},
                                        {"g_Output", Fsr3BindKind::kImage, 0}};
  const Def defs[] = {
      {P_PREPARE_INPUTS, "fsr3_prepare_inputs_cs", RENUT_HOST_SHADER(fsr3_prepare_inputs_cs), fsr3::k_prepare_inputs,
       uint32_t(std::size(fsr3::k_prepare_inputs))},
      {P_SPD, "fsr3_shading_change_pyramid_cs", RENUT_HOST_SHADER(fsr3_shading_change_pyramid_cs),
       fsr3::k_shading_change_pyramid, uint32_t(std::size(fsr3::k_shading_change_pyramid))},
      {P_SHADING_CHANGE, "fsr3_shading_change_cs", RENUT_HOST_SHADER(fsr3_shading_change_cs), fsr3::k_shading_change,
       uint32_t(std::size(fsr3::k_shading_change))},
      {P_PREPARE_REACTIVITY, "fsr3_prepare_reactivity_cs", RENUT_HOST_SHADER(fsr3_prepare_reactivity_cs),
       fsr3::k_prepare_reactivity, uint32_t(std::size(fsr3::k_prepare_reactivity))},
      {P_LUMA_INSTABILITY, "fsr3_luma_instability_cs", RENUT_HOST_SHADER(fsr3_luma_instability_cs),
       fsr3::k_luma_instability, uint32_t(std::size(fsr3::k_luma_instability))},
      {P_ACCUMULATE, "fsr3_accumulate_cs", RENUT_HOST_SHADER(fsr3_accumulate_cs), fsr3::k_accumulate,
       uint32_t(std::size(fsr3::k_accumulate))},
      {P_ACCUMULATE_NATIVE, "fsr3_accumulate_li_bil_cs", RENUT_HOST_SHADER(fsr3_accumulate_li_bil_cs),
       fsr3::k_accumulate_li_bil, uint32_t(std::size(fsr3::k_accumulate_li_bil))},
      {P_CLEAR, "fsr3_clear_cs", RENUT_HOST_SHADER(fsr3_clear_cs), kClearBinds, 2},
      {P_CLEAR_UINT, "fsr3_clear_uint_cs", RENUT_HOST_SHADER(fsr3_clear_uint_cs), kClearBinds, 2},
      {P_CLEAR_BUFFER, "fsr3_clear_buffer_cs", RENUT_HOST_SHADER(fsr3_clear_buffer_cs), kClearBufferBinds, 2},
      {P_VIEW, "fsr3_view_cs", RENUT_HOST_SHADER(fsr3_view_cs), kViewBinds, 3},
      {P_ACCUMULATE_SHARP, "fsr3_accumulate_sharp_cs", RENUT_HOST_SHADER(fsr3_accumulate_sharp_cs),
       fsr3::k_accumulate_sharp, uint32_t(std::size(fsr3::k_accumulate_sharp))},
      {P_ACCUMULATE_NATIVE_SHARP, "fsr3_accumulate_li_bil_sharp_cs", RENUT_HOST_SHADER(fsr3_accumulate_li_bil_sharp_cs),
       fsr3::k_accumulate_li_bil_sharp, uint32_t(std::size(fsr3::k_accumulate_li_bil_sharp))},
      {P_RCAS, "fsr3_rcas_cs", RENUT_HOST_SHADER(fsr3_rcas_cs), fsr3::k_rcas, uint32_t(std::size(fsr3::k_rcas))},
      {P_RCAS_ADAPT, "fsr3_rcas_adapt_cs", RENUT_HOST_SHADER(fsr3_rcas_adapt_cs), fsr3::k_rcas_adapt,
       uint32_t(std::size(fsr3::k_rcas_adapt))},
  };
  for (const Def &d : defs) {
    Pass &p = g3.passes[d.id];
    p.name = d.name;
    p.binds = d.binds;
    p.count = d.count;
    p.shader = CreateHostShader(s.device.get(), d.blob, d.name);
    if (!p.shader || !s.stream_convert_layout)
      return Fail(d.name);
    p.pipeline = s.device->createComputePipeline(
        plume::RenderComputePipelineDesc(s.stream_convert_layout.get(), p.shader.get(), 8, 8, 1));
    if (!p.pipeline)
      return Fail(d.name);
    p.pipeline->setName(d.name);
  }
  g3.linear_sampler = plume::Deko3DSamplerId(s.sampler_set.get(), kLinearClampSamplerSlot);
  g3.point_sampler = plume::Deko3DSamplerId(s.sampler_set.get(), kPointClampSamplerSlot);
  if (g3.linear_sampler == ~0u || g3.point_sampler == ~0u)
    return Fail("the samplers");
  return true;
}

bool PrepareResources(VideoState &s, uint32_t w, uint32_t h, uint32_t up_w, uint32_t up_h, plume::RenderFormat out) {
  if (g3.res[R_ACC1].texture && g3.render_w == w && g3.render_h == h && g3.up_w == up_w && g3.up_h == up_h &&
      g3.out_format == out)
    return true;
  for (Resource &r : g3.res)
    ReleaseResource(r);
  const uint32_t w2 = std::max(w / 2, 1u), h2 = std::max(h / 2, 1u);
  using F = plume::RenderFormat;
  struct D {
    Res r;
    F f;
    uint32_t w, h, levels;
  };
  const D d[] = {{R_ACC1, F::R8_UNORM, w, h, 1},           {R_ACC2, F::R8_UNORM, w, h, 1},
                 {R_LUMA1, F::R16_FLOAT, w, h, 1},         {R_LUMA2, F::R16_FLOAT, w, h, 1},
                 {R_FP16X1, F::R16_FLOAT, w, h, 1},        {R_SHADING_CHANGE, F::R8_UNORM, w2, h2, 1},
                 {R_NEW_LOCKS, F::R8_UNORM, up_w, up_h, 1}, {R_UPSCALED1, F::R16G16B16A16_FLOAT, up_w, up_h, 1},
                 {R_UPSCALED2, F::R16G16B16A16_FLOAT, up_w, up_h, 1}, {R_SPD_MIPS, F::R16G16_FLOAT, w2, h2, Levels(w2, h2)},
                 {R_FARTHEST_MIP1, F::R16_FLOAT, w2, h2, 1}, {R_LUMA_HIST1, F::R8G8B8A8_UNORM, w, h, 1},
                 {R_LUMA_HIST2, F::R8G8B8A8_UNORM, w, h, 1}, {R_DILATED_REACTIVE, F::R8G8B8A8_UNORM, w, h, 1},
                 {R_DILATED_MV, F::R16G16_FLOAT, w, h, 1},  {R_DILATED_DEPTH, F::R32_FLOAT, w, h, 1},
                 {R_OUTPUT, out, up_w, up_h, 1}};
  for (const D &x : d) {
    if (!MakeResource(s, g3.res[x.r], x.f, x.w, x.h, x.levels))
      return Fail("a texture");
  }
  if (g3.recon)
    Video::RetireLocked(std::move(g3.recon));
  g3.recon_words = w * h;
  g3.recon = s.device->createBuffer(
      plume::RenderBufferDesc::DefaultBuffer(uint64_t(g3.recon_words) * 4, plume::RenderBufferFlag::STORAGE));
  g3.recon_address = g3.recon ? g3.recon->getDeviceAddress() : 0;
  if (!g3.recon_address)
    return Fail("the reconstructed depth buffer");
  g3.render_w = w;
  g3.render_h = h;
  g3.up_w = up_w;
  g3.up_h = up_h;
  g3.out_format = out;
  g3.first = true;
  RGPU_INFO("[fsr3] textures for {}x{} -> {}x{}", w, h, up_w, up_h);
  for (uint32_t r = 0; r < R_COUNT; ++r) {
    const Resource &x = g3.res[r];
    RGPU_INFO("[fsr3] texture {}: {}x{} format {} levels {}: sampled slot {} id {}, storage slot {} id {}", r, x.width,
              x.height, uint32_t(x.format), x.levels, x.texture_slot, x.texture_id, x.image_slots[0], x.image_ids[0]);
  }
  return true;
}

// A shader's resource by name, as the port's ResolveName: the frame's ping-pong halves, the inputs, the output.
struct Frame {
  uint32_t color_id, depth_id, motion_id;
  uint64_t fsr_ubo, spd_ubo, dims_ubo, clear_ubo, view_ubo, rcas_ubo;
  uint32_t view_texture;  // the view pass's texture handle
};

uint32_t TextureHandle(uint32_t image_id, uint32_t sampler_id) { return (image_id & 0xFFFFFu) | (sampler_id << 20); }

// ~0u: unknown.
uint32_t Resolve(const char *n, const Frame &f, bool image, uint32_t level = 0) {
  const bool odd = g3.odd;
  const Res acc_r = odd ? R_ACC2 : R_ACC1, acc_w = odd ? R_ACC1 : R_ACC2;
  const Res up_r = odd ? R_UPSCALED2 : R_UPSCALED1, up_w = odd ? R_UPSCALED1 : R_UPSCALED2;
  const Res hist_r = odd ? R_LUMA_HIST2 : R_LUMA_HIST1, hist_w = odd ? R_LUMA_HIST1 : R_LUMA_HIST2;
  const Res luma_cur = odd ? R_LUMA2 : R_LUMA1, luma_prev = odd ? R_LUMA1 : R_LUMA2;
  auto own = [&](Res r, bool point = false) -> uint32_t {
    const Resource &x = g3.res[r];
    if (image)
      return x.image_ids[std::min(level, x.levels - 1)];
    return TextureHandle(x.texture_id, point ? g3.point_sampler : g3.linear_sampler);
  };
  if (!std::strncmp(n, "rw_spd_mip", 10)) {
    const uint32_t l = uint32_t(n[10] - '0');
    return l < g3.res[R_SPD_MIPS].levels ? g3.res[R_SPD_MIPS].image_ids[l] : ~0u;
  }
  if (!std::strcmp(n, "r_input_color_jittered"))
    return TextureHandle(f.color_id, g3.linear_sampler);
  if (!std::strcmp(n, "r_input_depth"))
    return TextureHandle(f.depth_id, g3.point_sampler);
  if (!std::strcmp(n, "r_input_motion_vectors"))
    return TextureHandle(f.motion_id, g3.point_sampler);
  if (!std::strcmp(n, "r_spd_mips"))
    return own(R_SPD_MIPS);
  if (!std::strcmp(n, "r_accumulation"))
    return own(acc_r);
  if (!std::strcmp(n, "rw_accumulation"))
    return own(acc_w);
  if (!std::strcmp(n, "r_internal_upscaled_color"))
    return own(up_r);
  if (!std::strcmp(n, "rw_internal_upscaled_color"))
    return own(up_w);
  if (!std::strcmp(n, "r_rcas_input"))  // tw2de (2026-11-07): this frame's upscaled colour (xc1fsr's, AMD's)
    return own(up_w);
  if (!std::strcmp(n, "r_luma_history"))
    return own(hist_r);
  if (!std::strcmp(n, "rw_luma_history"))
    return own(hist_w);
  if (!std::strcmp(n, "r_current_luma") || !std::strcmp(n, "rw_current_luma"))
    return own(luma_cur);
  if (!std::strcmp(n, "r_previous_luma"))
    return own(luma_prev);
  if (!std::strcmp(n, "r_luma_instability") || !std::strcmp(n, "rw_luma_instability"))
    return own(R_FP16X1);
  if (!std::strcmp(n, "r_farthest_depth_mip1") || !std::strcmp(n, "rw_farthest_depth_mip1"))
    return own(R_FARTHEST_MIP1);
  if (!std::strcmp(n, "r_shading_change") || !std::strcmp(n, "rw_shading_change"))
    return own(R_SHADING_CHANGE);
  if (!std::strcmp(n, "r_new_locks") || !std::strcmp(n, "rw_new_locks"))
    return own(R_NEW_LOCKS);
  if (!std::strcmp(n, "r_dilated_reactive_masks") || !std::strcmp(n, "rw_dilated_reactive_masks"))
    return own(R_DILATED_REACTIVE);
  if (!std::strcmp(n, "r_dilated_motion_vectors") || !std::strcmp(n, "rw_dilated_motion_vectors"))
    return own(R_DILATED_MV);
  if (!std::strcmp(n, "r_dilated_depth") || !std::strcmp(n, "rw_dilated_depth"))
    return own(R_DILATED_DEPTH, true);
  if (!std::strcmp(n, "rw_upscaled_output"))
    return own(R_OUTPUT);
  return ~0u;
}

// strips: the pass is built with XC2FSR_GROUP_STRIPS = strips (tools/fsr3/convert_fsr3.py): dispatched as (strips, gy,
// ceil(gx / strips)), so the GPU walks the frame in vertical strips of that many groups (texture cache locality; the
// shader maps the ids back and skips the groups past the right edge). 0: the usual (gx, gy, 1).
constexpr uint32_t kStrips = 16;  // XC2FSR_GROUP_STRIPS of prepare_inputs / prepare_reactivity (tools/fsr3/convert_fsr3.py)
bool Dispatch(plume::RenderCommandList *cmd, PassId id, const Frame &f, uint32_t gx, uint32_t gy,
              uint32_t image_override = ~0u, uint32_t strips = 0) {
  const Pass &p = g3.passes[id];
  // renut_gpu_profile: each pass's GPU time on its own line ("FSR 3 <pass>"), in the frames it times.
  if (gpu_profile::Timed()) {
    static uint32_t labels[P_COUNT] = {};
    if (!labels[id])
      labels[id] = gpu_profile::Label(std::string("FSR 3 ") + p.name);
    gpu_profile::Mark(State(), labels[id]);
  }
  plume::Deko3DRawBinds b;
  for (uint32_t i = 0; i < p.count; ++i) {
    const Fsr3Bind &x = p.binds[i];
    if (x.kind == Fsr3BindKind::kUbo) {
      uint64_t a = 0;
      uint32_t size = 0;
      if (!std::strcmp(x.name, "cbFSR3UPSCALER_t"))
        a = f.fsr_ubo, size = sizeof(FsrConstants);
      else if (!std::strcmp(x.name, "cbSPD_t"))
        a = f.spd_ubo, size = sizeof(SpdConstants);
      else if (!std::strcmp(x.name, "cbNvnDims_t"))
        a = f.dims_ubo, size = sizeof(NvnDims);
      else if (!std::strcmp(x.name, "Fsr3Clear"))
        a = f.clear_ubo, size = sizeof(ClearConstants);
      else if (!std::strcmp(x.name, "Fsr3View"))
        a = f.view_ubo, size = 64;
      else if (!std::strcmp(x.name, "cbRCAS_t"))
        a = f.rcas_ubo, size = 16;
      if (!a) {
        RGPU_WARN("[fsr3] {}: no data for {}", p.name, x.name);
        return false;
      }
      b.uboAddress[x.slot] = a;
      b.uboSize[x.slot] = size;
      b.uboCount = std::max(b.uboCount, uint32_t(x.slot) + 1);
      continue;
    }
    if (x.kind == Fsr3BindKind::kSsbo) {
      if (std::strcmp(x.name, "Fsr3ReconDepth_t") || !g3.recon_address) {
        RGPU_WARN("[fsr3] {}: no buffer for {}", p.name, x.name);
        return false;
      }
      b.ssboAddress[x.slot] = g3.recon_address;
      b.ssboSize[x.slot] = g3.recon_words * 4;
      b.ssboCount = std::max(b.ssboCount, uint32_t(x.slot) + 1);
      continue;
    }
    const bool image = x.kind == Fsr3BindKind::kImage;
    const uint32_t v = !std::strcmp(x.name, "g_Texture")  ? f.view_texture
                       : !std::strcmp(x.name, "g_Output") ? g3.res[R_OUTPUT].image_ids[0]
                       : image_override != ~0u           ? image_override
                                                         : Resolve(x.name, f, image);
    if (v == ~0u) {
      RGPU_WARN("[fsr3] {}: {} unresolved", p.name, x.name);
      return false;
    }
    if (image) {
      b.images[x.slot] = v;
      b.imageCount = std::max(b.imageCount, uint32_t(x.slot) + 1);
    } else {
      b.textures[x.slot] = v;
      b.textureCount = std::max(b.textureCount, uint32_t(x.slot) + 1);
    }
  }
  gx = std::max(gx, 1u);
  gy = std::max(gy, 1u);
  const bool strip = strips > 1 && gx > strips;
  if (!plume::Deko3DDispatchRaw(cmd, p.pipeline.get(), b, strip ? strips : gx, gy, strip ? (gx + strips - 1) / strips : 1))
    return false;
  static const bool light = !REXCVAR_GET(tw2de_diag_fsr3_full_barriers);
  plume::Deko3DComputeBarrier(cmd, light);
  return true;
}

uint64_t Upload(const void *data, uint32_t size) {
  const uint32_t bytes = (size + 255u) & ~255u;
  UploadAllocation a = Video::UploadsLocked().AllocateWithRoom(bytes, bytes, 256);
  if (!a)
    return 0;
  std::memset(a.cpu, 0, bytes);
  std::memcpy(a.cpu, data, size);
  return a.gpu_address;
}

// Clears every level of an own texture to 'value' (float bits, or a uint for R_RECON_DEPTH).
bool Clear(plume::RenderCommandList *cmd, Res r, const uint32_t value[4]) {
  const Resource &x = g3.res[r];
  for (uint32_t level = 0; level < x.levels; ++level) {
    ClearConstants c{};
    std::memcpy(c.value, value, sizeof(c.value));
    c.size[0] = std::max(x.width >> level, 1u);
    c.size[1] = std::max(x.height >> level, 1u);
    Frame f{};
    f.clear_ubo = Upload(&c, sizeof(c));
    if (!f.clear_ubo || !Dispatch(cmd, P_CLEAR, f, (c.size[0] + 7) / 8,
                                  (c.size[1] + 7) / 8, x.image_ids[level]))
      return false;
  }
  return true;
}

bool ClearF(plume::RenderCommandList *cmd, Res r, float a, float b = 0, float c = 0, float d = 0) {
  const uint32_t v[4] = {std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b), std::bit_cast<uint32_t>(c),
                         std::bit_cast<uint32_t>(d)};
  return Clear(cmd, r, v);
}

void NativeUpsampleConstants(const float jitter[2], float off_sq[12], float box_w[12], float lanczos_w[12]) {
  const float bx = -jitter[0], by = -jitter[1];
  const bool flip_col = bx > 0.0f, flip_row = by > 0.0f;
  const float x0 = bx + (flip_col ? 1.0f : -1.0f), x2 = bx + (flip_col ? -1.0f : 1.0f);
  const float y0 = by + (flip_row ? 1.0f : -1.0f), y2 = by + (flip_row ? -1.0f : 1.0f);
  const float xs0 = x0 * x0, xs2 = x2 * x2, ys0 = y0 * y0, ys2 = y2 * y2, bxs = bx * bx, bys = by * by;
  const float v[9] = {bxs + bys, xs0 + ys0, xs2 + ys0, xs0 + ys2, xs2 + ys2, xs0 + bys, xs2 + bys, bxs + ys0, bxs + ys2};
  for (int i = 0; i < 12; i++) {
    off_sq[i] = i < 9 ? v[i] : 0.0f;
    box_w[i] = i < 9 ? std::exp(-2.3f * v[i]) : 0.0f;
    const float x = i < 9 ? std::min(v[i], 4.0f) : 4.0f, b = 0.25f * x - 1.0f;
    lanczos_w[i] = i < 9 ? (b * b) * (b * x - b) : 0.0f;
  }
}

}  // namespace

bool Fsr3On() {
  static const bool on = REXCVAR_GET(tw2de_fsr3) != 0;
  return on && IsDeko3D();
}

void Fsr3FrameStartLocked() {
  if (!Fsr3On())
    return;
  g3.jitter_on = g3.ran_this_frame;
  g3.ran_this_frame = false;
  // ffxFsr3UpscalerGetJitterPhaseCount and GetJitterOffset: Halton(2, 3) over 8 x (output / render)^2 phases, in
  // pixels within [-0.5, 0.5]; in normalized device coordinates 2 x / width and -2 y / height.
  // tw2de (2026-11-07, dynamic resolution): the render size this frame draws (the textures' is the largest), and the
  // phase count stepped by one a frame towards the new ratio's (xc1fsr), not jumping with each change.
  uint32_t rw = g3.render_w ? g3.render_w : 1024, rh = g3.render_h ? g3.render_h : 576;
  if (DrsSized()) {
    // TW2 (2026-11-15, dynamic resolution's levels as target sizes): this frame's level's size (the scene's targets
    // are made at it; DrsFrameStartLocked chose it before this). The jitter is half a pixel of that size.
    const RenderScale level = DrsSceneScale();
    rw = level.Size(rw);
    rh = level.Size(rh);
  } else if (DrsOn()) {
    rw = DrsDrawn(rw);
    rh = DrsDrawn(rh);
  }
  const uint32_t uw = g3.up_w ? g3.up_w : 1280;
  const float ratio = float(uw) / float(rw);
  const uint32_t phases = std::max(uint32_t(8.0f * ratio * ratio), 1u);
  if (g3.first || !g3.phase_count)
    g3.phase_count = phases;
  else if (phases > g3.phase_count)
    ++g3.phase_count;
  else if (phases < g3.phase_count)
    --g3.phase_count;
  g3.phase = (g3.phase + 1) % g3.phase_count;
  const float jx = Halton(g3.phase + 1, 2) - 0.5f, jy = Halton(g3.phase + 1, 3) - 0.5f;
  g3.prev_jitter[0] = g3.jitter[0];
  g3.prev_jitter[1] = g3.jitter[1];
  g3.jitter[0] = jx;
  g3.jitter[1] = jy;
  g3.jitter_ndc[0] = 2.0f * jx / float(rw);
  g3.jitter_ndc[1] = -2.0f * jy / float(rh);
  static const bool no_jitter = REXCVAR_GET(tw2de_diag_fsr3_no_jitter);
  if (!g3.jitter_on || no_jitter || (g3.diag_flags & 1u)) {
    // Not drawn jittered: FSR's constants say so too (no jitter this frame).
    g3.jitter[0] = g3.jitter[1] = 0.0f;
    g3.jitter_ndc[0] = g3.jitter_ndc[1] = 0.0f;
  }
}

uint32_t Fsr3DiagFlags() { return g3.diag_flags; }

void Fsr3JitterNdc(float out[2]) {
  out[0] = g3.jitter_ndc[0];
  out[1] = g3.jitter_ndc[1];
}

void Fsr3OutputBackLocked();

bool Fsr3UpscaleLocked(uint32_t color_image, uint32_t width, uint32_t height, HostSurface *target,
                       const Fsr3SceneInputs &scene, bool copy_to_target, uint32_t out_width,
                       uint32_t out_height) {
  VideoState &s = State();
  if (!Fsr3On() || g3.failed || !target || !target->texture || target->depth || !scene.motion || !scene.depth ||
      !scene.depth->texture || !scene.single_sample || !s.command_list) {
    // tw2de (2026-11-07): why, now and then (a 1280x720 scene's FSR did not run, silently)
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(10)) {
      last = now;
      RGPU_INFO("[fsr3] not run: failed {} target {} motion {} depth {} single-sample {} (scene {}x{}, picture {}x{})",
                g3.failed, target != nullptr, scene.motion != nullptr, scene.depth != nullptr, scene.single_sample,
                scene.width, scene.height, width, height);
    }
    return false;
  }
  if (scene.width != width || scene.height != height) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("[fsr3] the upscale's picture is {}x{}, the scene {}x{}: the game's upscale instead", width, height,
                scene.width, scene.height);
    }
    return false;
  }
  if (target->format != plume::RenderFormat::R8G8B8A8_UNORM || target->samples != plume::RenderSampleCount::COUNT_1) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("[fsr3] the upscale's target (format {}, {} samples) is not single-sampled RGBA8: the game's upscale",
                uint32_t(target->format), uint32_t(target->samples));
    }
    return false;
  }
  const bool own_size = !copy_to_target && out_width && out_height;  // tw2de (2026-11-08)
  const uint32_t up_w = own_size ? out_width : std::max(target->scale.Size(target->width), 1u);
  const uint32_t up_h = own_size ? out_height : std::max(target->scale.Size(target->height), 1u);
  // tw2de (2026-11-07, tw2de_render_height): a scene larger than the output (1600x900 or 1920x1080 in hand,
  // 1280x720 output) is not upscaled: the game's draw scales it down.
  if (width > up_w || height > up_h) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_INFO("[fsr3] the scene {}x{} is larger than the output {}x{}: the game's downscale instead", width, height,
                up_w, up_h);
    }
    return false;
  }
  // tw2de (2026-11-07, dynamic resolution): the textures at the largest render size (the inputs' host size), the
  // render size as drawn this frame (xc1fsr: its maxRenderW/H and renderSize).
  const uint32_t max_w = std::max(scene.max_width, width), max_h = std::max(scene.max_height, height);
  Fsr3OutputBackLocked();  // tw2de (2026-11-08): from the last frame's copies (Fsr3CopyOutputLocked)
  g3.output_ready = false;
  if (!PreparePasses(s) || !PrepareResources(s, max_w, max_h, up_w, up_h, target->format))
    return false;
  plume::RenderCommandList *cmd = s.command_list;
  // The inputs' sampled descriptors.
  // The depth through the surface's own sampled view (its depth plane: render/surfaces.cpp).
  const uint32_t depth_slot = SurfaceSampleSlotLocked(scene.depth);
  if (g3.motion_slot == kInvalidDescriptor)
    g3.motion_slot = Video::AllocateTextureSlotLocked();
  if (depth_slot == kInvalidDescriptor || g3.motion_slot == kInvalidDescriptor)
    return Fail("the inputs' descriptors");
  if (g3.motion_in != scene.motion) {
    Video::SetTextureSlotLocked(g3.motion_slot, scene.motion, nullptr);
    g3.motion_in = scene.motion;
  }
  Frame f{};
  f.color_id = color_image & 0xFFFFFu;
  f.depth_id = plume::Deko3DImageId(s.texture_set.get(), depth_slot);
  f.motion_id = plume::Deko3DImageId(s.texture_set.get(), g3.motion_slot);
  // The scene's targets are read now: their layouts for reading, then the work before waited for.
  TransitionSurfaceLocked(scene.depth, plume::RenderTextureLayout::SHADER_READ);
  cmd->setFramebuffer(nullptr);
  cmd->barriers(plume::RenderBarrierStage::COMPUTE,
                plume::RenderTextureBarrier(scene.motion, plume::RenderTextureLayout::SHADER_READ));
  // The renderer's bindless descriptor sets bound (the handles index them).
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  plume::Deko3DComputeBarrier(cmd);

  // Constants (the port's RunFsr; ffx_fsr3upscaler.cpp's setupDeviceDepthToViewSpaceDepthParams).
  // tw2de (2026-11-08, the shimmer after the pause): the history started again (a cut) and the next run at another
  // render size starts it again too. Back from the pause or a menu (drawn at full size: not 3D gameplay), the first frame
  // is a cut at the full size and the next one at dynamic resolution's lower size; continued from there, the static
  // picture shimmered (jitter's sub-pixel steps visible) until FSR's history was reset (tw2de_fsr3_diag flag 32).
  // At start-up the same two frames came out as two cuts (no shimmer).
  const bool size_after_reset =
      g3.last_reset && (g3.prev_render[0] != int32_t(width) || g3.prev_render[1] != int32_t(height));
  const bool reset = g3.first || scene.cut || size_after_reset || (g3.diag_flags & 32u);  // 32: diagnostic
  g3.last_reset = reset;
  FramesDiag(scene, reset);
  FsrConstants c{};
  c.iRenderSize[0] = int32_t(width);
  c.iRenderSize[1] = int32_t(height);
  c.iPreviousFrameRenderSize[0] = g3.prev_render[0] ? g3.prev_render[0] : int32_t(width);
  c.iPreviousFrameRenderSize[1] = g3.prev_render[1] ? g3.prev_render[1] : int32_t(height);
  c.iUpscaleSize[0] = int32_t(up_w);
  c.iUpscaleSize[1] = int32_t(up_h);
  c.iPreviousFrameUpscaleSize[0] = g3.prev_up[0] ? g3.prev_up[0] : int32_t(up_w);
  c.iPreviousFrameUpscaleSize[1] = g3.prev_up[1] ? g3.prev_up[1] : int32_t(up_h);
  c.iMaxRenderSize[0] = int32_t(max_w);
  c.iMaxRenderSize[1] = int32_t(max_h);
  c.iMaxUpscaleSize[0] = int32_t(up_w);
  c.iMaxUpscaleSize[1] = int32_t(up_h);
  c.fJitter[0] = g3.jitter[0];
  c.fJitter[1] = g3.jitter[1];
  c.fPreviousFrameJitter[0] = g3.prev_jitter[0];
  c.fPreviousFrameJitter[1] = g3.prev_jitter[1];
  // The motion target holds previous minus current position in normalized device coordinates: in UV units x / 2 and
  // -y / 2 (UV's y runs down).
  c.fMotionVectorScale[0] = (g3.diag_flags & 8u) ? 0.0f : 0.5f;
  c.fMotionVectorScale[1] = (g3.diag_flags & 8u) ? 0.0f : -0.5f;
  c.fDownscaleFactor[0] = float(width) / float(up_w);
  c.fDownscaleFactor[1] = float(height) / float(up_h);
  // The projection: z_ndc = (A z + B) / -z (right-handed, D3D's 0..1): near = B / A, far = B / (A + 1).
  const float A = scene.proj_a, B = scene.proj_b;
  float near_z = 0.1f, far_z = 5000.0f;
  if (A < -0.5f && B < 0.0f && A + 1.0f < 0.0f) {
    near_z = B / A;
    far_z = B / (A + 1.0f);
  }
  if (scene.near_z > 0.0f && scene.far_z > scene.near_z) {  // TW2 (2026-11-15): given
    near_z = scene.near_z;
    far_z = scene.far_z;
  }
  const float cot_y = scene.proj_y > 0 ? scene.proj_y : 2.94591f;
  const float fov_y = 2.0f * std::atan(1.0f / cot_y);
  const float aspect = float(width) / float(height);
  {
    const float fmin = std::min(near_z, far_z), fmax = std::max(near_z, far_z);
    const float q = fmax / (fmin - fmax);
    c.fDeviceToViewDepth[0] = -1.0f * q;
    c.fDeviceToViewDepth[1] = q * fmin;
    c.fDeviceToViewDepth[2] = 1.0f / (cot_y / aspect);
    c.fDeviceToViewDepth[3] = 1.0f / cot_y;
  }
  c.fTanHalfFOV = std::tan(fov_y * 0.5f);
  c.fViewSpaceToMetersFactor = 1.0f;
  c.fJitterSequenceLength = float(g3.phase_count);
  const auto now = std::chrono::steady_clock::now();
  const float dt = g3.last_run == std::chrono::steady_clock::time_point{}
                       ? 1.0f / 30.0f
                       : std::chrono::duration<float>(now - g3.last_run).count();
  g3.last_run = now;
  c.fDeltaTime = std::clamp(dt, 0.0f, 1.0f);
  c.fDeltaPreExposure = 1.0f;
  g3.frame_index = reset ? 0.0f : g3.frame_index + 1.0f;
  c.fFrameIndex = g3.frame_index;
  c.fVelocityFactor = 1.0f;
  c.fReactivenessScale = 1.0f;
  c.fShadingChangeScale = 1.0f;
  c.fAccumulationAddedPerFrame = 1.0f / 3.0f;
  c.fMinDisocclusionAccumulation = -1.0f / 3.0f;

  // SPD (ffxSpdSetup) over the half size.
  const uint32_t w2 = std::max(width / 2, 1u), h2 = std::max(height / 2, 1u);
  SpdConstants spd{};
  {
    const uint32_t end_x = (w2 - 1) / 64, end_y = (h2 - 1) / 64;
    const uint32_t gx = end_x + 1, gy = end_y + 1;
    spd.numWorkGroups = gx * gy;
    spd.mips = std::min(uint32_t(std::floor(std::log2(float(std::max(w2, h2))))), 12u);
    spd.renderSize[0] = width;
    spd.renderSize[1] = height;
  }
  NvnDims dims{};
  {
    const Resource &m = g3.res[R_SPD_MIPS];
    for (uint32_t level = 0; level < 6; ++level) {
      dims.spdMipDims[level / 2][(level & 1) * 2 + 0] = int32_t(std::max(m.width >> level, 1u));
      dims.spdMipDims[level / 2][(level & 1) * 2 + 1] = int32_t(std::max(m.height >> level, 1u));
    }
    dims.maskDims[0] = dims.maskDims[1] = dims.maskDims[2] = dims.maskDims[3] = 1;
    dims.farthestDims[0] = int32_t(g3.res[R_FP16X1].width);
    dims.farthestDims[1] = int32_t(g3.res[R_FP16X1].height);
    dims.farthestDims[2] = int32_t(g3.res[R_FARTHEST_MIP1].width);
    dims.farthestDims[3] = int32_t(g3.res[R_FARTHEST_MIP1].height);
    dims.mvDecode[0] = dims.mvDecode[1] = 1.0f;  // the texel as it is (then fMotionVectorScale)
    dims.mvEncoding[0] = 0.0f;                   // not Xenoblade 2's RGB10A2 velocity
    // TW2 (2026-11-15): the depth input's value to 0 (near) .. 1 (far) (fsr3_prepare_inputs_cs.glsl).
    dims.mvEncoding[2] = scene.depth_scale;
    dims.mvEncoding[3] = scene.depth_bias;
    // tw2de (2026-11-07): the depth texture's own size (xc1fsr's TextureGetWidth / Height of it).
    dims.depthDims[0] = int32_t(std::max(scene.depth->scale.Size(scene.depth->width), 1u));
    dims.depthDims[1] = int32_t(std::max(scene.depth->scale.Size(scene.depth->height), 1u));
    NativeUpsampleConstants(g3.jitter, dims.nativeOffSq, dims.nativeBoxW, dims.nativeLanczosW);
    const float rw = float(width), rh = float(height);
    const float prw = float(c.iPreviousFrameRenderSize[0]), prh = float(c.iPreviousFrameRenderSize[1]);
    // tw2de (2026-11-07): [6], [7] one over the largest render size (xc1fsr's UniformConstants).
    const float u[24] = {1.0f / float(up_w), 1.0f / float(up_h), c.fJitter[0] / rw, c.fJitter[1] / rh,
                         rw, rh, 1.0f / float(max_w), 1.0f / float(max_h),
                         float(width / 2), float(height / 2), 1.0f / float(dims.farthestDims[2]),
                         1.0f / float(dims.farthestDims[3]),
                         prw, prh, c.fPreviousFrameJitter[0] / prw, c.fPreviousFrameJitter[1] / prh,
                         1.0f / rw, 1.0f / rh, float(c.iPreviousFrameUpscaleSize[0]), float(c.iPreviousFrameUpscaleSize[1]),
                         1.0f / float(up_w), 1.0f / float(up_h), 0.1f / c.fJitterSequenceLength, 0.0f};
    std::memcpy(dims.u, u, sizeof(u));
  }
  f.fsr_ubo = Upload(&c, sizeof(c));
  f.spd_ubo = Upload(&spd, sizeof(spd));
  f.dims_ubo = Upload(&dims, sizeof(dims));
  if (!f.fsr_ubo || !f.spd_ubo || !f.dims_ubo)
    return false;
  // tw2de (2026-11-07): RCAS's constant, as xc1fsr: exp2(-stops), adaptive at -0.25 stops (its ADAPTIVE_SHARPNESS),
  // fixed clamped to [-0.25, 0.5].
  static const int32_t sharpening = REXCVAR_GET(tw2de_fsr3_sharpening);
  const bool sharpen = sharpening != 0;
  if (sharpen) {
    const float stops = sharpening == 2 ? -0.25f : std::clamp(float(REXCVAR_GET(tw2de_fsr3_sharpness)), -0.25f, 0.5f);
    const uint32_t rcas[4] = {std::bit_cast<uint32_t>(std::exp2(-stops)), 0, 0, 0};
    f.rcas_ubo = Upload(rcas, sizeof(rcas));
    if (!f.rcas_ubo)
      return false;
  }

  bool ok = true;
  static const bool skip_passes = REXCVAR_GET(tw2de_diag_fsr3_skip_passes);
  const uint32_t dst_x = (up_w + 7) / 8, dst_y = (up_h + 7) / 8;
  if (!skip_passes) {
  if (reset) {
    ok = ok && ClearF(cmd, R_ACC1, 0) && ClearF(cmd, R_ACC2, 0) && ClearF(cmd, R_LUMA1, 0) && ClearF(cmd, R_LUMA2, 0) &&
         ClearF(cmd, R_LUMA_HIST1, 0) && ClearF(cmd, R_LUMA_HIST2, 0) && ClearF(cmd, R_UPSCALED1, 0) &&
         ClearF(cmd, R_UPSCALED2, 0) && ClearF(cmd, R_SPD_MIPS, 0) && ClearF(cmd, R_NEW_LOCKS, 0) &&
         ClearF(cmd, R_SHADING_CHANGE, 0);
  }
  {
    // The reconstructed previous depth: the far plane (1.0) where nothing reprojects (cleared every frame).
    ClearConstants cc{};
    cc.value[0] = 0x3F800000u;
    cc.size[0] = g3.recon_words;
    Frame cf{};
    cf.clear_ubo = Upload(&cc, sizeof(cc));
    ok = ok && cf.clear_ubo && Dispatch(cmd, P_CLEAR_BUFFER, cf, (g3.recon_words + 63) / 64, 1);
  }
  const uint32_t src_x = (width + 7) / 8, src_y = (height + 7) / 8;
  const uint32_t sc_x = (uint32_t(width * 0.5f) + 7) / 8, sc_y = (uint32_t(height * 0.5f) + 7) / 8;
  const uint32_t spd_x = (w2 - 1) / 64 + 1, spd_y = (h2 - 1) / 64 + 1;
  const bool native = width == up_w && height == up_h;
  ok = ok && Dispatch(cmd, P_PREPARE_INPUTS, f, src_x, src_y, ~0u, kStrips) && Dispatch(cmd, P_SPD, f, spd_x, spd_y) &&
       Dispatch(cmd, P_SHADING_CHANGE, f, sc_x, sc_y) &&
       Dispatch(cmd, P_PREPARE_REACTIVITY, f, src_x, src_y, ~0u, kStrips) &&
       (native || Dispatch(cmd, P_LUMA_INSTABILITY, f, src_x, src_y)) &&
       Dispatch(cmd, native ? (sharpen ? P_ACCUMULATE_NATIVE_SHARP : P_ACCUMULATE_NATIVE)
                            : (sharpen ? P_ACCUMULATE_SHARP : P_ACCUMULATE), f, dst_x, dst_y);
  // tw2de (2026-11-07): RCAS from this frame's upscaled colour into the output (64 threads a 16x16 tile, groups in
  // strips of 16: XC2FSR_GROUP_STRIPS in its build).
  if (ok && sharpen)
    ok = Dispatch(cmd, sharpening == 2 ? P_RCAS_ADAPT : P_RCAS, f, (up_w + 15) / 16, (up_h + 15) / 16, ~0u, kStrips);
  }
  if (!ok)
    return Fail("a pass");
  // tw2de_fsr3_diag: a texture shown as the output.
  static const bool diag = REXCVAR_GET(tw2de_fsr3_diag);
  if (skip_passes)
    g3.view = 1;  // tw2de_diag_fsr3_skip_passes: the input as the output
  if (diag || skip_passes) {
    if (diag && now - g3.view_read >= std::chrono::seconds(1)) {
      g3.view_read = now;
      int v = g3.view;
      float scale = 0.0f, bias = 0.0f;
      unsigned flags = 0;
      if (FILE *vf = std::fopen(tw2de::renut_gpu::NextToExecutable("fsr3_view").string().c_str(), "r")) {
        const int n = std::fscanf(vf, "%d %f %f %u", &v, &scale, &bias, &flags);
        if (n < 4)
          flags = 0;
        if (n < 1)
          v = g3.view;
        if (n < 2)
          scale = 0.0f;
        if (n < 3)
          bias = 0.0f;
        std::fclose(vf);
      }
      if (v != g3.view || scale != g3.view_scale || bias != g3.view_bias || flags != g3.diag_flags)
        RGPU_INFO("[fsr3] view {} scale {} bias {} flags {} (tw2de_fsr3_diag)", v, scale, bias, flags);
      g3.diag_flags = flags;
      g3.view = v;
      g3.view_scale = scale;
      g3.view_bias = bias;
    }
    if (g3.view > 0) {
      struct View {
        float scale[4], bias[4];
        uint32_t size[4], mode[4];
      } vw{};
      vw.scale[0] = vw.scale[1] = vw.scale[2] = 1.0f;
      const char *name = nullptr;
      Res res = R_COUNT;
      const bool odd = g3.odd;
      switch (g3.view) {
        case 1: name = "r_input_color_jittered"; break;
        case 2: name = "r_input_motion_vectors"; vw.mode[0] = 1; vw.scale[0] = 25.0f; break;
        case 3: name = "r_input_depth"; vw.scale[0] = vw.scale[1] = vw.scale[2] = 50.0f;
                vw.bias[0] = vw.bias[1] = vw.bias[2] = -49.0f; break;
        case 4: res = R_DILATED_MV; vw.mode[0] = 1; vw.scale[0] = 50.0f; break;
        case 5: res = R_DILATED_DEPTH; vw.scale[0] = vw.scale[1] = vw.scale[2] = 50.0f;
                vw.bias[0] = vw.bias[1] = vw.bias[2] = -49.0f; break;
        case 6: res = R_DILATED_REACTIVE; break;
        case 7: res = R_FP16X1; break;
        case 8: res = R_NEW_LOCKS; break;
        case 9: res = odd ? R_UPSCALED1 : R_UPSCALED2; break;
        case 10: res = odd ? R_LUMA2 : R_LUMA1; break;
        case 11: res = odd ? R_ACC1 : R_ACC2; break;
        case 12: res = R_FARTHEST_MIP1; vw.scale[0] = vw.scale[1] = vw.scale[2] = 0.01f; break;
        default: break;
      }
      if (g3.view_scale != 0.0f) {
        vw.scale[0] = vw.scale[1] = vw.scale[2] = g3.view_scale;
        vw.bias[0] = vw.bias[1] = vw.bias[2] = g3.view_bias;
      }
      uint32_t tw = width, th = height;
      if (name) {
        f.view_texture = Resolve(name, f, false);
      } else if (res != R_COUNT) {
        f.view_texture = TextureHandle(g3.res[res].texture_id, g3.point_sampler);
        tw = g3.res[res].width;
        th = g3.res[res].height;
      } else {
        f.view_texture = ~0u;
      }
      if (f.view_texture != ~0u) {
        vw.size[0] = up_w;
        vw.size[1] = up_h;
        vw.size[2] = tw;
        vw.size[3] = th;
        f.view_ubo = Upload(&vw, sizeof(vw));
        if (f.view_ubo)
          Dispatch(cmd, P_VIEW, f, dst_x, dst_y);
      }
    }
  }
  // The output into the draw's target.
  // tw2de (2026-11-08): not when the caller copies it itself (Fsr3CopyOutputLocked: FSR before the
  // post-processing copies it straight into the textures the post-processing reads; the 1920x1080 copy into the
  // target and those textures' copies from it were 2 + ~2 ms a frame).
  g3.output_ready = !copy_to_target;
  if (!copy_to_target) {
    g3.output_layout_general = true;
  } else {
  if (gpu_profile::Timed()) {
    static const uint32_t copy_label = gpu_profile::Label("FSR 3 copy to target");
    gpu_profile::Mark(s, copy_label);
  }
  {
    plume::RenderTextureBarrier b[2] = {
        plume::RenderTextureBarrier(g3.res[R_OUTPUT].texture.get(), plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(target->texture.get(), plume::RenderTextureLayout::COPY_DEST)};
    cmd->barriers(plume::RenderBarrierStage::COPY, nullptr, 0, b, 2);
    target->layout = plume::RenderTextureLayout::COPY_DEST;
  }
  const plume::RenderBox box(0, 0, int32_t(up_w), int32_t(up_h));
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(target->texture.get()),
                         plume::RenderTextureCopyLocation::Subresource(g3.res[R_OUTPUT].texture.get()), 0, 0, 0, &box);
  cmd->barriers(plume::RenderBarrierStage::COMPUTE,
                plume::RenderTextureBarrier(g3.res[R_OUTPUT].texture.get(), plume::RenderTextureLayout::GENERAL));
  TransitionSurfaceLocked(target, plume::RenderTextureLayout::COLOR_WRITE);
  ++target->writes;  // tw2de_renut_resolve_skip: the target changed
  }
  g3.prev_render[0] = int32_t(width);
  g3.prev_render[1] = int32_t(height);
  g3.prev_up[0] = int32_t(up_w);
  g3.prev_up[1] = int32_t(up_h);
  g3.odd = !g3.odd;
  g3.first = false;
  ++g3.frames;
  g3.ran_this_frame = true;
  if (!g3.logged) {
    g3.logged = true;
    RGPU_INFO("[fsr3] FSR 3.1 upscales {}x{} -> {}x{} (near {:.3g}, far {:.4g}, vertical FOV {:.3g} degrees, {} jitter "
              "phases)",
              width, height, up_w, up_h, near_z, far_z, fov_y * 57.2958f, g3.phase_count);
  }
  return true;
}

bool Fsr3CopyOutputLocked(plume::RenderTexture *dest, uint32_t x, uint32_t y, const plume::RenderBox &box) {
  VideoState &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  if (!g3.output_ready || !dest || !cmd || !g3.res[R_OUTPUT].texture)
    return false;
  if (gpu_profile::Timed()) {
    static const uint32_t copy_label = gpu_profile::Label("FSR 3 copy to textures");
    gpu_profile::Mark(s, copy_label);
  }
  plume::RenderTextureBarrier b[2] = {
      plume::RenderTextureBarrier(g3.res[R_OUTPUT].texture.get(), plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(dest, plume::RenderTextureLayout::COPY_DEST)};
  cmd->barriers(plume::RenderBarrierStage::COPY, nullptr, 0, b, 2);
  g3.output_layout_general = false;
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dest, 0, 0),
                         plume::RenderTextureCopyLocation::Subresource(g3.res[R_OUTPUT].texture.get()), x, y, 0, &box);
  return true;
}

// tw2de (2026-11-08): the output back to the passes' layout before FSR's next run (Fsr3CopyOutputLocked's copies).
void Fsr3OutputBackLocked() {
  VideoState &s = State();
  if (g3.output_layout_general || !s.command_list || !g3.res[R_OUTPUT].texture)
    return;
  s.command_list->barriers(plume::RenderBarrierStage::COMPUTE,
                           plume::RenderTextureBarrier(g3.res[R_OUTPUT].texture.get(),
                                                       plume::RenderTextureLayout::GENERAL));
  g3.output_layout_general = true;
}

}  // namespace renut::gpu

#else  // !PLUME_DEKO3D

namespace renut::gpu {
bool Fsr3On() { return false; }
void Fsr3FrameStartLocked() {}
void Fsr3JitterNdc(float out[2]) { out[0] = out[1] = 0.0f; }
bool Fsr3UpscaleLocked(uint32_t, uint32_t, uint32_t, HostSurface *, const Fsr3SceneInputs &, bool, uint32_t,
                       uint32_t) {
  return false;
}
bool Fsr3CopyOutputLocked(plume::RenderTexture *, uint32_t, uint32_t, const plume::RenderBox &) { return false; }
}  // namespace renut::gpu

#endif

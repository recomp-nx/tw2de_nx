/**
 * @file    gpu/render/graphics_options.cpp
 */
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/render/surfaces.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_set>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/types.h>
#ifdef __SWITCH__
#include <switch.h>

#include <rex/ui/switch_saltynx.h>  // tw2de (2026-11-07): BaseMode, Reverse-NX's mode
#endif

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/dynamic_resolution.h"  // tw2de (2026-11-07)

REXCVAR_DEFINE_STRING(renut_shadow_resolution, "Resolution", "Nuts&Bolts/Graphics",
                      "Shadow map resolution, as a multiple of the console's: Resolution follows "
                      "renut_resolution; 1x is the console's; 2x, 3x and 4x are sharper.")
    .allowed({"Resolution", "1x", "2x", "3x", "4x"});

REXCVAR_DEFINE_STRING(renut_ambient_occlusion, "Half", "Nuts&Bolts/Graphics",
                      "Ambient occlusion (SSAO) resolution: Half the screen's, as on the console, "
                      "or Full.")
    .allowed({"Half", "Full"});

REXCVAR_DEFINE_STRING(renut_anisotropic_filtering, "Game", "Nuts&Bolts/Graphics",
                      "Anisotropic texture filtering: the game's own, or at least 2x, 4x, 8x or "
                      "16x.")
    .allowed({"Game", "2x", "4x", "8x", "16x"});

REXCVAR_DEFINE_STRING(renut_post_process_precision, "High", "Nuts&Bolts/Graphics",
                      "Precision of the bloom, depth of field and SSAO buffers: Game keeps the "
                      "console's 8 bits (faint glow shows as blobs sliding over dark areas), High "
                      "uses 16 bits.")
    .allowed({"Game", "High"});

REXCVAR_DEFINE_INT32(tw2de_diag_output, 0, "Game/Diagnostics",
                     "The output's size (2026-11-07): 0 automatic (docked: the console's display resolution, 1920x1080 "
                     "on a 1080p TV, the 1280x720 targets then drawn at 1.5x; in hand 1280x720), 1 always 1280x720, "
                     "2 always 1920x1080 (a test in hand). With a 1280x720 scene the picture is upscaled when it is shown "
                     "(the boot menu's Upscaler), the game's targets stay 1280x720")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de {
uint32_t RenderHeight();  // cpu/tw2de_render_resolution.cpp
}

REXCVAR_DEFINE_INT32(tw2de_diag_scene_scale, 0, "Game/Diagnostics",
                     "Test (2026-11-07, a dynamic resolution's survey): the scene's render targets drawn at this host "
                     "scale (RenderScale codes: 1 = 2/3, 2 = 3/2, 3 = 2, 0 = off) on deko3d, whose shaders are made for "
                     "1x; the output's 1280x720 targets and the shadow maps stay as they are")
    .range(0, 7)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {

bool Fsr3On();

namespace {

// D3DFORMAT low bits of D3DFMT_D24S8 (the shadow maps; the scene's depth is
// D24FS8).
constexpr u32 kFormatD24S8 = 22;
// The SSAO targets' EDRAM base (sub_8235FA90).
constexpr u32 kAmbientOcclusionEdramBase = 1864;
// The game's screen height (widescreen.h).
constexpr u32 kGuestScreenHeight = 720;

bool IsShadowMap(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  if (width != height)
    return false;
  if ((u32(desc.Format) & 0x3F) == kFormatD24S8)
    return width == 512 || width == 768 || width == 1024;
  // The local lights' depth, converted for their atlas.
  return !d3d::IsDepthFormat(desc.Format) && width == 512 &&
         GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF) == GpuColorFormat::Fmt16_16_Float;
}

bool IsAmbientOcclusion(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  return !d3d::IsDepthFormat(desc.Format) && width == 640 && height == 360 &&
         (u32(desc.ColorInfo) & 0xFFF) == kAmbientOcclusionEdramBase;
}

// A quarter of the screen or less: the post-processing buffers (depth of
// field and bloom, 320x180 down to 20x11 at 1280x720). Their blurs average a
// few taps 2 of their own texels apart, made for buffers box-downsampled from
// the screen; rendered at a higher scale, the buffers keep detail those taps
// step over, so edges come out as ghost copies rather than blurred.
bool IsPostProcessBuffer(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  return !d3d::IsDepthFormat(desc.Format) && width * 4 <= GuestScreenWidth() &&
         height * 4 <= kGuestScreenHeight;
}

// Half the screen or less, single-sampled, 8_8_8_8: the bloom's bright pass
// (640x360) and the post-processing buffers below it, and the SSAO targets.
bool IsLowPrecisionPostTarget(const d3d::SurfaceDesc &desc) {
  const u32 width = d3d::SurfaceWidth(desc.Size);
  const u32 height = d3d::SurfaceHeight(desc.Size);
  return !d3d::IsDepthFormat(desc.Format) && ((u32(desc.SurfaceInfo) >> 16) & 3) == 0 &&
         width * 2 <= GuestScreenWidth() && height * 2 <= kGuestScreenHeight &&
         GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF) == GpuColorFormat::Fmt8_8_8_8;
}

// The options as LatchGraphicsOptionsLocked last read them: the draw path
// asks for every target and sampler of every draw, too often for the
// settings' strings.
struct Latched {
  bool shadow_follows_resolution = true;
  RenderScale shadow_scale;
  bool full_ambient_occlusion = false;
  uint32_t min_anisotropy = 1;
  bool high_precision_post = true;
  bool output_1080p = false;  // tw2de (2026-11-07): Output1080p
};
Latched g_latched;

} // namespace

void LatchGraphicsOptionsLocked() {
  const std::string &shadows = REXCVAR_GET(renut_shadow_resolution);
  g_latched.shadow_follows_resolution = shadows == "Resolution" || shadows.empty();
  g_latched.shadow_scale = {};
  if (!g_latched.shadow_follows_resolution)
    RenderScale::FromRatio(uint32_t(std::atoi(shadows.c_str())), 1, g_latched.shadow_scale);
  g_latched.full_ambient_occlusion = REXCVAR_GET(renut_ambient_occlusion) == "Full";
  // tw2de (2026-10-18): deko3d draws every target at the console's scale (its shaders are made for 1x, pass scale1
  // of tools/deko3d_glsl_opt.py): shadow maps follow the resolution (1x) and the ambient occlusion is the game's.
  if (IsDeko3D()) {
    g_latched.shadow_follows_resolution = REXCVAR_GET(tw2de_diag_scene_scale) == 0;
    g_latched.shadow_scale = {};
    g_latched.full_ambient_occlusion = false;
  }
  const int anisotropy = std::atoi(REXCVAR_GET(renut_anisotropic_filtering).c_str());
  g_latched.min_anisotropy = anisotropy >= 2 ? uint32_t(anisotropy) : 1;
  g_latched.high_precision_post = REXCVAR_GET(renut_post_process_precision) != "Game";
}

#ifdef __SWITCH__
// tw2de (2026-11-07): docked, whether the console's display resolution (appletGetDefaultDisplayResolution: the TV
// output the system settings chose, 1920x1080 or 1280x720) is 1080p. Asked again when its change event
// (appletGetDefaultDisplayResolutionChangeEvent) is signalled, when the mode changes, and once a second as well: the
// window code (sdk windowed_app_context_switch.cpp) waits on the same event and clears it, so a signal can be missed
// here. As the window code: the applet's answer when it agrees with the mode (Reverse-NX's "docked" in hand gets
// 1280x720 from it: then the mode's 1920x1080).
static bool DisplayIs1080p(bool docked) {
  static Event changed{};
  static bool has_event = false, asked = false, tried = false, last_docked = false, is_1080p = true;
  static u64 asked_at = 0;
  static s32 logged_w = -1, logged_h = -1;
  if (!tried) {
    tried = true;
    has_event = R_SUCCEEDED(appletGetDefaultDisplayResolutionChangeEvent(&changed));
  }
  const u64 now = armGetSystemTick();
  bool ask = !asked || docked != last_docked || now - asked_at > armGetSystemTickFreq();
  if (has_event && R_SUCCEEDED(eventWait(&changed, 0))) {
    eventClear(&changed);
    ask = true;
  }
  if (!ask)
    return is_1080p;
  asked = true;
  last_docked = docked;
  asked_at = now;
  s32 w = 0, h = 0;
  bool v = true;  // docked without an answer: the dock's usual 1080p
  if (!docked)
    v = false;
  else if (R_SUCCEEDED(appletGetDefaultDisplayResolution(&w, &h)) && w > 0 && h > 0 && w >= 1920)
    v = w >= 1920 && h >= 1080;
  else if (w > 0 && h > 0 && appletGetOperationMode() == AppletOperationMode_Console)
    v = false;  // really docked, the TV at 720p (or 480p): 1280x720
  if (v != is_1080p || w != logged_w || h != logged_h) {
    logged_w = w;
    logged_h = h;
    REXLOG_INFO("[output] display resolution {}x{} ({}), {}: output {}", w, h,
                has_event ? "its change event watched" : "no change event", docked ? "docked" : "handheld",
                v ? "1920x1080" : "1280x720");
  }
  is_1080p = v;
  return v;
}
#endif

// tw2de (2026-11-07, tw2de_diag_output): whether the output is 1920x1080 now (docked at a 1080p display
// resolution), read at each frame's latch.
static bool Output1080pNow() {
  static const int32_t mode = REXCVAR_GET(tw2de_diag_output);
  static const bool able = mode != 1 && IsDeko3D();
  if (!able)
    return false;
  if (mode == 2)
    return true;
#ifdef __SWITCH__
  // Reverse-NX's mode when its overlay rules ("Controlled by system" No), else the console's: "docked" in hand gives
  // 1080p (the panel shows it scaled down), "handheld" in the dock 720p (the TV scales it up).
  const bool docked = rex::ui::switch_saltynx::BaseMode(appletGetOperationMode() == AppletOperationMode_Console);
  const bool display_1080p = DisplayIs1080p(docked);  // the console's display resolution
  return docked && display_1080p;
#else
  return false;
#endif
}

bool LatchOutput1080pLocked() {
  const bool now = Output1080pNow();
  if (now == g_latched.output_1080p)
    return false;
  g_latched.output_1080p = now;
  REXLOG_INFO("[output] {}", now ? "1920x1080 (the 1280x720 targets at 1.5x)" : "1280x720");
  return true;
}

bool Output1080p() { return g_latched.output_1080p; }

namespace {
std::unordered_set<uint64_t> g_promoted;  // OutputTargetKey of the 1280x720 scene's targets drawn at 1.5x
}

uint64_t OutputTargetKey(const d3d::SurfaceDesc &desc) {
  return uint64_t(u32(desc.ColorInfo) & 0xFFFu) | uint64_t(u32(desc.SurfaceInfo) & 0x3FFFFu) << 12 |
         uint64_t(u32(desc.Format)) << 32;
}

bool g_output_phase = false;

bool SetOutputPhaseLocked(bool after_copy) {
  if (g_output_phase == after_copy)
    return false;
  g_output_phase = after_copy;
  return true;
}

bool PromoteOutputTargetLocked(uint64_t out_key) {
  if (!g_promoted.insert(out_key).second)
    return false;
  REXLOG_INFO("[output] a 1280x720 target ({:016X}) of the 1280x720 scene's output drawn at 1920x1080 from now on",
              out_key);
  return true;
}

// tw2de (2026-11-08): a header of the scene's size (tw2de_render_height, 16:9), or, another scene than 1280x720
// tiling, a column or row of the scene's size (a target built at the extent: 6D907A14's 800x900 header, the
// post-processing's composite at 1600x900, drawn in two halves while the scene tiles; it stayed at the dynamic
// resolution, the picture soft).
bool OutputSceneSized(uint32_t width, uint32_t height) {
  const u32 rh = tw2de::RenderHeight(), rw = rh * 16 / 9;
  if (width == rw && height == rh)
    return true;
  if (rh == 720)
    return false;
  uint32_t tw = 0, th = 0;
  TilingExtentLocked(tw, th);
  return tw == rw && th == rh && (height == th || width == tw);
}

RenderScale SurfaceRenderScale(const d3d::SurfaceDesc &desc, RenderScale resolution) {
  // tw2de (2026-11-07, dynamic resolution): the scene's targets (multisampled, and the rest but the shadow maps, the
  // post-processing buffers and the output's 1280x720 ones) are drawn at the frame's dynamic resolution
  // (dynamic_resolution.h).
  RenderScale scene = resolution;
  // TW2 (2026-11-14, tw2de_drs_sized): the frame's level as the scale itself (render_scale.h kDrsFirst).
  const bool sized = DrsSized();
  if (sized)
    scene = DrsSceneScale();
  else
    scene.drs = DrsOn();
  if ((u32(desc.SurfaceInfo) >> 16) & 3)
    return scene;  // multisampled: the scene's
  const u32 width = d3d::SurfaceWidth(desc.Size);
  const u32 height = d3d::SurfaceHeight(desc.Size);
  // TW2 (2026-11-14): TW2's shadow maps are never the scene's: square (512x512 and 384x384 with a 32-bit
  // float colour target, which IsShadowMap below does not know) or taller than wide (the sun's three 768x768 cascades
  // in one 768x2304 depth target).
  if (sized && height >= width)
    return {};
  // tw2de (2026-11-07, dynamic resolution): a 1280x720 scene's 1280x720 targets drawn after its picture's copy (the
  // copy's, the HUD's: promoted, as for a 1920x1080 output below) are the output's at a 1280x720 output too: not
  // dynamic (the copy scales the drawn part up to them).
  if ((scene.drs || sized) && !g_latched.output_1080p && width == 1280 && height == 720 && tw2de::RenderHeight() == 720 &&
      g_output_phase && g_promoted.count(OutputTargetKey(desc)))
    return {};
  // tw2de (2026-11-08): a 1600x900 / 1024x576 / 1920x1080 scene's targets of its size promoted in the output phase
  // (FSR 3.1 before its post-processing, draw.cpp EarlyFsrBeforePostLocked): the output's size (6/5, 15/8, 1; 5/4 for
  // 1024x576 at 1280x720).
  if (const u32 rh = tw2de::RenderHeight(); rh != 720 && OutputSceneSized(width, height) && g_output_phase &&
                                                 g_promoted.count(OutputTargetKey(desc))) {
    RenderScale s;
    if (RenderScale::FromRatio(g_latched.output_1080p ? 1080u : 720u, rh, s))
      return s;
  }
  // tw2de (2026-11-07): with a 1280x720 scene too (the HUD at 1080p): its scene targets are multisampled on the
  // guest (2x, also with MSAA off or FSR 3.1 on the host: the first test above), so only what follows the game's
  // upscale draw (1:1 there) is 1280x720 single-sampled.
  if (g_latched.output_1080p && width == 1280 && height == 720 &&
      (tw2de::RenderHeight() != 720 || (g_output_phase && g_promoted.count(OutputTargetKey(desc))))) {
    RenderScale s;
    RenderScale::FromRatio(3, 2, s);
    return s;
  }
  // tw2de (2026-11-07): the output's 1280x720 targets after a scene of another size keep the output's scale.
  if (width == 1280 && height == 720 && tw2de::RenderHeight() != 720)
    return {};
  if (IsShadowMap(desc, width, height))
    return g_latched.shadow_follows_resolution ? resolution : g_latched.shadow_scale;
  if (g_latched.full_ambient_occlusion && IsAmbientOcclusion(desc, width, height))
    return resolution.Times(2);
  if (IsPostProcessBuffer(desc, width, height))
    return {};  // the console's
  return scene;
}

plume::RenderFormat SurfaceColorFormat(const d3d::SurfaceDesc &desc) {
  // 16-bit unorm keeps 8-bit's clamping to [0, 1], which the game's blends
  // and shaders were made for.
  if (g_latched.high_precision_post && IsLowPrecisionPostTarget(desc))
    return plume::RenderFormat::R16G16B16A16_UNORM;
  return ColorTargetFormat(GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF));
}

uint32_t MinAnisotropy() {
  return g_latched.min_anisotropy;
}

} // namespace renut::gpu

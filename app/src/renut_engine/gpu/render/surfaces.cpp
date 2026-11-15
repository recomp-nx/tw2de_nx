/**
 * @file    gpu/render/surfaces.cpp
 */
#include "renut_engine/gpu/render/surfaces.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/video.h"
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // TW2 (2026-11-13): Deko3DCreateTextureRows
#endif
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp
// tw2de (2026-11-11, diagnostic): how many kept (0: kUnusedTargetMax) and a log of the targets built and dropped.
REXCVAR_DEFINE_INT32(tw2de_renut_kept_targets, 0, "Game/Diagnostics",
                     "tw2de: unused render targets kept (0: 24); with log_diagnostics, the targets built and dropped "
                     "are logged ([targets], 600 lines at most)");
static uint32_t g_target_log_lines = 0;
static bool TargetLogOn() { return REXCVAR_GET(log_diagnostics) && g_target_log_lines < 600; }

REXCVAR_DEFINE_STRING(renut_resolution, "720p", "Nuts&Bolts/Graphics",
                      "Internal render resolution, by its height: 480p, 720p (the game's own), "
                      "1080p, 1440p or 4K (2160 lines). Applies from the next frame.")
    .allowed({"480p", "720p", "1080p", "1440p", "4K"});
REXCVAR_DEFINE_BOOL(tw2de_renut_keep_unused_targets, true, "Game/Enhancements",
                    "Renut's renderer (2026-10-22): a render target no surface uses any more is kept for a moment "
                    "for a surface that takes the same EDRAM view again, instead of being destroyed and made anew. "
                    "the game switches one surface between two views every frame (8 image creations a frame without "
                    "this). 2026-10-24: at most 4 targets for at most 250 ms (it was 24 for 10 s, and the intro "
                    "movie hung with that in v85); 2026-11-08: at most 8 (5 views switched a frame at 1600x900). "
                    "false = destroyed at once, as before");

namespace renut::gpu {

namespace {

// ResolutionScale, once latched.
RenderScale g_resolution_scale;
bool g_resolution_latched = false;

struct FramebufferKey {
  const HostSurface *colors[d3d::kMaxRenderTargets] = {};
  uint64_t color_generations[d3d::kMaxRenderTargets] = {};
  const HostSurface *depth = nullptr;
  uint64_t depth_generation = 0;

  bool operator==(const FramebufferKey &o) const {
    for (uint32_t i = 0; i < d3d::kMaxRenderTargets; ++i) {
      if (colors[i] != o.colors[i] || color_generations[i] != o.color_generations[i])
        return false;
    }
    return depth == o.depth && depth_generation == o.depth_generation;
  }
};

struct CachedFramebuffer {
  FramebufferKey key;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
};

// A D3DSurface is a view of EDRAM: its base tile (RB_COLOR_INFO /
// RB_DEPTH_INFO bits 0-11), pitch (RB_SURFACE_INFO bits 0-13) and sample
// count. Surface objects over the same EDRAM with the same layout see the same
// pixels on the GPU, and N&B relies on it: the depth surface its after-effects
// pass binds is a second object over the scene's depth buffer (the character
// holograms test against it), and each hologram renders through surfaces of
// its own at the same EDRAM bases. So host targets belong to the EDRAM view,
// shared by every surface object over it, as long as they also agree on the
// host format and render scale: graphics_options renders some surfaces at
// another scale (the post-processing buffers at 1x), and a view used by
// surfaces of both kinds would otherwise be rebuilt at every switch.
uint64_t TargetKey(const d3d::SurfaceDesc &desc, bool depth, plume::RenderFormat format,
                   RenderScale scale) {
  const u32 info = desc.SurfaceInfo;
  return uint64_t(u32(desc.ColorInfo) & 0xFFF) | uint64_t(info & 0x3FFF) << 12 |
         uint64_t((info >> 16) & 3) << 26 | uint64_t(depth) << 28 |
         uint64_t(scale.code & 15) << 29 | uint64_t(u32(format)) << 33 |  // tw2de (2026-11-08): 4 bits (codes 8, 9)
         uint64_t(scale.drs) << 62 |  // tw2de (2026-11-07, dynamic resolution: another target)
         uint64_t(scale.code >> 4) << 56;  // TW2 (2026-11-14): the code's bits 4 and 5 (the levels' codes, 12-32)
}

std::unordered_map<uint64_t, std::unique_ptr<HostSurface>> g_targets;
// Surface VA -> its target's key, and how many surfaces use each target.
std::unordered_map<u32, uint64_t> g_surface_keys;
std::unordered_map<uint64_t, uint32_t> g_target_users;
// tw2de (2026-10-22, tw2de_renut_keep_unused_targets): targets no surface uses any more, kept (by key, with when
// the last user left) for a surface that takes the same view again. The game rewrites one surface object between two
// views every frame (its 1024x576 2x scene depth and a 1280x720 one): each switch retired one target and made the
// other anew, about 8 image creations a frame (250 GPU mappings a second, 4 % of a core in the kernel's mapping
// calls). The console's EDRAM keeps what each view held, so a kept target is also closer to it than a new one.
std::unordered_map<uint64_t, int64_t> g_unused_targets;
std::vector<CachedFramebuffer> g_framebuffers;
// tw2de (2026-10-19, render-thread proposal 8): the last framebuffers looked up, checked before the search of every
// framebuffer made (a linear search with an 80-byte compare at every draw's BindTargets); emptied whenever a cached
// framebuffer is retired (g_framebuffer_epoch).
struct RecentFramebuffer {
  FramebufferKey key;
  plume::RenderFramebuffer *framebuffer = nullptr;
  uint64_t epoch = 0;
};
RecentFramebuffer g_recent_framebuffers[4];
uint64_t g_framebuffer_epoch = 1;
uint64_t g_next_generation = 1;
}  // namespace
extern bool g_load_log_active;  // draw.cpp (tw2de_diag_load_draw_log)
namespace {
// tw2de (2026-10-19, tw2de_deko3d_depth24): the D3D depth formats each EDRAM depth view has been bound as (1 D24S8,
// 2 D24FS8). The host format is part of a target's key, so a view used as both would get two targets where the console
// has one memory: once a view has been bound as D24FS8, every surface over it takes the 32-bit float format.
std::unordered_map<uint64_t, uint8_t> g_depth_view_formats;
// SetTilingExtentLocked; 0 x 0 when not tiling.
uint32_t g_tiling_width = 0;
uint32_t g_tiling_height = 0;

// Drops cached framebuffers that reference 'surface'.
void ForgetFramebuffers(const HostSurface *surface) {
  for (size_t i = 0; i < g_framebuffers.size();) {
    const FramebufferKey &key = g_framebuffers[i].key;
    bool uses = key.depth == surface;
    for (const HostSurface *color : key.colors)
      uses = uses || color == surface;
    if (uses) {
      Video::RetireLocked(std::move(g_framebuffers[i].framebuffer));
      g_framebuffers[i] = std::move(g_framebuffers.back());
      g_framebuffers.pop_back();
      ++g_framebuffer_epoch;
    } else {
      ++i;
    }
  }
}

// Retires the target and everything made over it (deferred past in-flight
// frames).
// tw2de (2026-10-19, CPU proposal 8, tw2de_renut_cpu_memos): GetHostSurfaceLocked's result per surface VA and the
// three descriptor words it reads, valid while nothing it reads has changed (g_surface_epoch: bumped by every target
// built, retired, unused or rebound, the resolution scale latched and the tiling extent set).
struct SurfaceMemo {
  // tw2de (2026-11-11): color_info (the EDRAM base) too. LR at 1600x900 (two 800x900 tiles) points one surface
  // object at its scene's colour (base 0x340... of the tile) and then, the same size, pitch and format, at its depth's
  // EDRAM for the per-tile depth restore (ps 9172595C7ECD7A5D writes the depth's words as colour): without the base
  // the memo gave the scene's target, the depth words were drawn over the scene's left tile (the green left half).
  u32 va = 0, size = 0, info = 0, format = 0, color_info = 0;
  uint64_t epoch = 0;
  HostSurface *surface = nullptr;
};
SurfaceMemo g_surface_memo[1024];  // tw2de (2026-10-19, render-thread proposal 8): indexed by a hash of the VA; TW2 (2026-11-12): 1024, not 64 (TW2 binds more surfaces: ~440 slot misses a frame with 64)
uint64_t g_surface_epoch = 1;

void FreeDepthViews(std::vector<DepthImage::View> &views) {
  for (DepthImage::View &v : views) {
    if (v.slot != UINT32_MAX && v.slot != kInvalidDescriptor)
      Video::FreeTextureSlotLocked(v.slot);
    Video::RetireLocked(std::move(v.view));
    if (v.rows_texture)
      Video::RetireLocked(std::move(v.rows_texture));  // (owns no memory: the image's goes with the image)
  }
  views.clear();
}

void RetireTarget(HostSurface &surface) {
  if (surface.handed)  // TW2 (2026-11-13): colour targets too (tw2de_colour_handover)
    DepthHandOverBeforeRetireLocked(&surface);  // tw2de (2026-10-19, tw2de_deko3d_depth_handover)
  FreeDepthViews(surface.handover_views);
  surface.stencil_used = false;
  ++g_surface_epoch;
  ForgetFramebuffers(&surface);
  if (surface.sample_slot != UINT32_MAX)
    Video::FreeTextureSlotLocked(surface.sample_slot);
  surface.sample_slot = UINT32_MAX;
  Video::RetireLocked(std::move(surface.sample_view));
  if (surface.stencil_slot != UINT32_MAX && surface.stencil_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(surface.stencil_slot);
  surface.stencil_slot = UINT32_MAX;
  Video::RetireLocked(std::move(surface.stencil_view));
  Video::RetireLocked(std::move(surface.texture));
}

}  // namespace (reopened below)
}  // namespace renut::gpu
REXCVAR_DECLARE(int32_t, tw2de_diag_scene_scale);  // graphics_options.cpp (2026-11-07)
REXCVAR_DEFINE_BOOL(tw2de_diag_msaa_off, false, "Game/Diagnostics",
                    "tw2de diagnostic (2026-10-22): the game's multisampled render targets are made single-sampled "
                    "(its 2x scene target and depth); their resolves then take the single-sample paths")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// tw2de (2026-11-06): MSAA 4x / 8x (the boot menu's Anti-aliasing row). The game's multisampled targets are its 2x
// scene target and depth; on the host they get this many samples instead (4x targets keep at least 4). Everything
// that depends on the count follows the host's: the pipelines' multisample state (keyed by it), the hardware resolves
// (all samples averaged), the one-sample resolves and depth resolves (the guest's sample index, valid at any host count:
// sample 0 and 1 are real samples there too), the EDRAM carry (per count), and the draws' sample mask, whose guest bits
// draw.cpp spreads over the host samples (MakeKey).
REXCVAR_DEFINE_INT32(tw2de_msaa_samples, 2, "Game",
                     "Samples of the game's multisampled render targets on the host: 2 (the game's own MSAA 2x), 4 or 8 "
                     "(the boot menu's MSAA 4x / 8x)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace renut::gpu {

// The guest's sample count of a surface (RB_SURFACE_INFO's MSAA field): 1, 2 or 4.
uint32_t GuestSampleCount(u32 info_word) {
  switch ((info_word >> 16) & 3) {
  case 1:
    return 2;
  case 2:
    return 4;
  default:
    return 1;
  }
}

namespace {

plume::RenderSampleCounts HostSampleCount(uint32_t samples) {
  return samples >= 8 ? plume::RenderSampleCount::COUNT_8
       : samples >= 4 ? plume::RenderSampleCount::COUNT_4
       : samples >= 2 ? plume::RenderSampleCount::COUNT_2
                      : plume::RenderSampleCount::COUNT_1;
}

plume::RenderSampleCounts SampleCount(u32 info_word) {
  if (REXCVAR_GET(tw2de_diag_msaa_off))
    return plume::RenderSampleCount::COUNT_1;
  const uint32_t guest = GuestSampleCount(info_word);
  if (guest == 1)
    return plume::RenderSampleCount::COUNT_1;
  static const uint32_t wanted = [] {
    const int32_t v = REXCVAR_GET(tw2de_msaa_samples);
    return v >= 8 ? 8u : v >= 4 ? 4u : 2u;
  }();
  return HostSampleCount(std::max(guest, wanted));
}

// At least 'min_width' x 'min_height' (see SetTilingExtentLocked), at 'scale'.
bool BuildTarget(HostSurface &surface, const d3d::SurfaceDesc &desc, uint32_t min_width,
                 uint32_t min_height, RenderScale scale) {
  surface.size_word = desc.Size;
  surface.info_word = desc.SurfaceInfo;
  surface.format_word = desc.Format;
  surface.width = std::max(d3d::SurfaceWidth(surface.size_word), min_width);
  surface.height = std::max(d3d::SurfaceHeight(surface.size_word), min_height);
  surface.scale = scale;
  std::fill(std::begin(surface.profile_labels), std::end(surface.profile_labels), UINT32_MAX);
  std::fill(std::begin(surface.resolve_labels), std::end(surface.resolve_labels), UINT32_MAX);
  const uint32_t host_width = surface.scale.Size(surface.width);
  const uint32_t host_height = surface.scale.Size(surface.height);
  surface.depth = d3d::IsDepthFormat(surface.format_word);
  surface.samples = SampleCount(surface.info_word);
  if (surface.depth) {
    surface.format = DepthTargetFormat(surface.format_word);  // tw2de (2026-10-19, tw2de_deko3d_depth24)
    if (tw2de::renut_gpu::Depth24On()) {
      // As GetHostSurfaceSlow keys it: a view ever bound as D24FS8 is 32-bit float for every surface over it.
      const auto it = g_depth_view_formats.find(TargetKey(desc, true, plume::RenderFormat::UNKNOWN, scale));
      if (it != g_depth_view_formats.end() && (it->second & 2) && !DepthF24UnormOn())  // TW2 (2026-11-13)
        surface.format = DepthTargetFormat();
    }
    // tw2de (2026-11-12, diagnostic): each depth target's D3D format (Fmt24_8 unorm or the 20e4 float) and why it
    // is 32-bit float when it is (its own format, or its EDRAM view bound as D24FS8 before).
    if (REXCVAR_GET(log_diagnostics)) {
      static uint32_t logged_all = 0;
      const bool own_float = DepthTargetFormat(surface.format_word) == DepthTargetFormat();
      if (logged_all++ < 32)
        RGPU_INFO("[depthfmt] {:08X} {}x{} x{}: d3d format word {:08X} (low 6 bits {}), host {}{}", surface.guest_va,
                  surface.width, surface.height, u32(surface.samples), surface.format_word, surface.format_word & 0x3F,
                  surface.format == DepthTargetFormat() ? "32-bit float" : "24-bit unorm",
                  surface.format != DepthTargetFormat() ? ""
                  : own_float ? " (its own format)" : " (its EDRAM view was bound as D24FS8)");
    }
    if (surface.format != DepthTargetFormat()) {
      static uint32_t logged = 0;
      if (logged++ < 16)
        if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("Depth target {:08X}: {}x{} x{} samples, 24-bit unorm depth with stencil (tw2de_deko3d_depth24)",
                  surface.guest_va, surface.width, surface.height, u32(surface.samples));
    }
  } else {
    surface.format = SurfaceColorFormat(desc);
  }

  plume::RenderDevice *device = Video::Device();
  const plume::RenderMultisampling multisampling(surface.samples);
  // Sampled too: resolves read them, and so does scan-out of a bound target.
  if (surface.depth) {
    surface.texture = device->createTexture(plume::RenderTextureDesc::DepthTarget(
        host_width, host_height, surface.format, multisampling));
  } else {
    surface.texture = device->createTexture(plume::RenderTextureDesc::ColorTarget(
        host_width, host_height, surface.format, multisampling));
  }
  surface.layout = plume::RenderTextureLayout::UNKNOWN;
  surface.generation = g_next_generation++;
  if (!TextureCreated(surface.texture.get())) {
    surface.texture.reset();
    if (!ReportDeviceLost(device)) {
      RGPU_ERROR("Surface {:08X}: {}x{} {} target creation failed", surface.guest_va,
                 host_width, host_height, surface.depth ? "depth" : "color");
    }
    return false;
  }
  surface.texture->setName(surface.depth ? "renut depth surface" : "renut color surface");
  RGPU_DEBUG("Surface {:08X}: {}x{} (x{:.3g} scale) {} format {} x{} samples", surface.guest_va,
             surface.width, surface.height, surface.scale.Factor(), surface.depth ? "depth" : "color",
             u32(surface.format), u32(surface.samples));
  return true;
}

} // namespace

uint64_t SurfaceEpochLocked() { return g_surface_epoch; }
void BumpSurfaceEpochLocked() { ++g_surface_epoch; }  // tw2de (2026-11-07): surfaces.h  // tw2de (2026-10-20, from sa2-nx, tw2de_renut_chain_reuse)
uint64_t FramebufferEpochLocked() { return g_framebuffer_epoch; }  // tw2de (2026-10-20, from sa2-nx, tw2de_renut_chain_reuse)

RenderScale ResolutionScale() {
  return g_resolution_scale;
}

void LatchResolutionScaleLocked() {
  LatchGraphicsOptionsLocked();
  const std::string resolution = REXCVAR_GET(renut_resolution);
  // Codes of RenderScale.
  RenderScale scale{resolution == "480p"    ? 1u
                    : resolution == "1080p" ? 2u
                    : resolution == "1440p" ? 3u
                    : resolution == "4K"    ? 4u
                                            : 0u};
  // tw2de (2026-10-18): the deko3d renderer draws at the console's resolution only; its shaders are made for it
  // (tools/deko3d_glsl_opt.py, pass scale1: no per-texture scale divisor, resolution_scale 1).
  if (IsDeko3D() && scale.code != 0) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      RGPU_WARN("renut_resolution = \"{}\" is not taken on deko3d: it draws at the console's 1280x720", resolution);
    }
    scale = {};
  }
  // tw2de (2026-11-07, a dynamic resolution's survey): the scene's targets at another host scale on deko3d (the
  // output's 1280x720 ones and the shadow maps stay: graphics_options.cpp).
  if (IsDeko3D() && REXCVAR_GET(tw2de_diag_scene_scale) > 0)
    scale = RenderScale{uint32_t(REXCVAR_GET(tw2de_diag_scene_scale))};
  if (SetOutputPhaseLocked(false))
    ++g_surface_epoch;  // tw2de (2026-11-07): a new frame begins before the scene's copy (graphics_options.h)
  if (LatchOutput1080pLocked() && g_resolution_latched)
    ++g_surface_epoch;  // tw2de (2026-11-07): the 1280x720 targets' scale changed
  if (!g_resolution_latched || scale != g_resolution_scale) {
    if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("Render resolution {}x{} ({:.3g}x)", scale.Size(GuestScreenWidth()), scale.Size(720),
              scale.Factor());
    g_resolution_scale = scale;
    ++g_surface_epoch;
    g_resolution_latched = true;
  }
}

void TilingExtentLocked(uint32_t &width, uint32_t &height) {
  width = g_tiling_width;
  height = g_tiling_height;
}

void SetTilingExtentLocked(uint32_t width, uint32_t height) {
  if (g_tiling_width != width || g_tiling_height != height)
    ++g_surface_epoch;
  g_tiling_width = width;
  g_tiling_height = height;
}

int64_t TargetNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void DropTarget(uint64_t key) {
  if (auto it = g_targets.find(key); it != g_targets.end()) {
    if (TargetLogOn() && it->second) {
      ++g_target_log_lines;
      RGPU_INFO("[targets] drop {:08X} key {:016X} {}x{} {} (frame {})", it->second->guest_va, key, it->second->width,
                it->second->height, it->second->depth ? "depth" : "colour", TextureFrameLocked());
    }
    RetireTarget(*it->second);
    g_targets.erase(it);
  }
}

// tw2de (2026-10-22): kept targets unused for kUnusedTargetMs, and the oldest beyond kUnusedTargetMax, go.
// tw2de (2026-10-24): 250 ms and 4 (were 10 s and 24, the v85 intro hang): the views the game switches between come
// back within a frame, and nothing is kept long enough to matter for memory or for what a view held. Swept every frame
// too (SweepUnusedTargetsLocked), not only when another target goes unused.
constexpr int64_t kUnusedTargetMs = 250;
// tw2de (2026-11-08): 8 (was 4). A 1600x900 scene in the field switches its two surface objects between 5 unused
// views a frame (the scene's tile colour and depth, the HUD's 1280x720 colour and depth, a 1024x1024 depth): with 4
// kept, the oldest went every time and about 5 targets were made anew every frame (156 a second, measured s196).
// tw2de (2026-11-11): 24 (was 8). LR at 1600x900 uses more views a frame (its tiles' colour, the depth's colour
// view, the scene's 64-bit and 32-bit views, the HUD's): with 8 kept, the scene's targets were dropped and built again
// every frame (the log's [targets] lines); with 32 none were.
constexpr size_t kUnusedTargetMax = 24;

void SweepUnusedTargets() {
  if (g_unused_targets.empty())
    return;
  const int64_t now = TargetNowMs();
  for (auto it = g_unused_targets.begin(); it != g_unused_targets.end();) {
    if (now - it->second > kUnusedTargetMs) {
      DropTarget(it->first);
      it = g_unused_targets.erase(it);
    } else {
      ++it;
    }
  }
  const size_t kept_max = REXCVAR_GET(tw2de_renut_kept_targets) > 0
                              ? size_t(REXCVAR_GET(tw2de_renut_kept_targets))
                              : kUnusedTargetMax;
  while (g_unused_targets.size() > kept_max) {
    auto oldest = std::min_element(g_unused_targets.begin(), g_unused_targets.end(),
                                   [](const auto &a, const auto &b) { return a.second < b.second; });
    DropTarget(oldest->first);
    g_unused_targets.erase(oldest);
  }
}

// A target gets a user again: no longer a kept one.
void UseTarget(uint64_t key) {
  if (++g_target_users[key] == 1 && !g_unused_targets.empty())
    g_unused_targets.erase(key);
}

// A surface stops using its target (destroyed, or its header now describes
// another view); the last user retires it (tw2de: or keeps it, tw2de_renut_keep_unused_targets).
void UnuseTarget(uint64_t key) {
  auto users = g_target_users.find(key);
  if (users == g_target_users.end() || --users->second)
    return;
  g_target_users.erase(users);
  ++g_surface_epoch;
  if (REXCVAR_GET(tw2de_renut_keep_unused_targets) && g_targets.count(key)) {
    g_unused_targets[key] = TargetNowMs();
    SweepUnusedTargets();
    return;
  }
  DropTarget(key);
}

void SweepUnusedTargetsLocked() { SweepUnusedTargets(); }

HostSurface *FindHostSurfaceLocked(u32 surface_va) {
  auto key = g_surface_keys.find(surface_va);
  if (key == g_surface_keys.end())
    return nullptr;
  auto it = g_targets.find(key->second);
  return it != g_targets.end() ? it->second.get() : nullptr;
}

void ColorAliasesLocked(const HostSurface *surface, std::vector<HostSurface *> &out) {
  out.clear();
  if (!surface || surface->depth)
    return;
  // TargetKey: the view in the low 32 bits, the host format above.
  constexpr uint64_t kViewMask = 0xFFFFFFFFull;
  uint64_t own = 0;
  bool found = false;
  for (const auto &[key, target] : g_targets) {
    if (target.get() == surface) {
      own = key;
      found = true;
      break;
    }
  }
  if (!found)
    return;
  for (const auto &[key, target] : g_targets) {
    if (target && target.get() != surface && target->texture && !target->depth &&
        (key & kViewMask) == (own & kViewMask))
      out.push_back(target.get());
  }
}

HostSurface *GetHostSurfaceSlow(u32 surface_va, const d3d::SurfaceDesc *desc);

namespace {
const SurfaceSnapshot *g_surface_snapshot = nullptr;
}  // namespace

void SnapSurfaces(const u8 *device_bytes, SurfaceSnapshot &out) {
  for (u32 i = 0; i < 5; ++i) {
    const u32 offset = i < 4 ? d3d::kDeviceOffsetRenderTargets + i * 4 : d3d::kDeviceOffsetDepthStencil;
    const u32 va = *reinterpret_cast<const be_u32 *>(device_bytes + offset);
    out.va[i] = va;
    const auto *desc = va ? GuestPtr<const d3d::SurfaceDesc>(va + sizeof(d3d::Resource)) : nullptr;
    out.valid[i] = desc != nullptr;
    if (desc)
      std::memcpy(out.desc[i], desc, sizeof(out.desc[i]));
  }
}

void SetSurfaceSnapshotLocked(const SurfaceSnapshot *snapshot) {
  g_surface_snapshot = snapshot;
}

const d3d::SurfaceDesc *SurfaceDescLocked(u32 surface_va) {
  if (!surface_va)
    return nullptr;
  if (const SurfaceSnapshot *snap = g_surface_snapshot) {
    for (u32 i = 0; i < 5; ++i) {
      if (snap->valid[i] && snap->va[i] == surface_va)
        return reinterpret_cast<const d3d::SurfaceDesc *>(snap->desc[i]);
    }
  }
  return GuestPtr<const d3d::SurfaceDesc>(surface_va + sizeof(d3d::Resource));
}

HostSurface *GetHostSurfaceLocked(u32 surface_va) {
  if (!surface_va)
    return nullptr;
  // Targets made before the first Swap are of the configured scale too.
  if (!g_resolution_latched)
    LatchResolutionScaleLocked();
  const d3d::SurfaceDesc *desc = SurfaceDescLocked(surface_va);
  if (!desc)
    return nullptr;

  const bool memo = tw2de::renut_gpu::CpuMemosOn();
  SurfaceMemo &m = g_surface_memo[(surface_va * 0x9E3779B1u) >> 22];
  if (memo && m.epoch == g_surface_epoch && m.va == surface_va && m.size == desc->Size &&
      m.info == desc->SurfaceInfo && m.format == desc->Format && m.color_info == desc->ColorInfo) {
    // the same as the lookup's: the surface bound now
    m.surface->size_word = desc->Size;
    m.surface->info_word = desc->SurfaceInfo;
    m.surface->format_word = desc->Format;
    return m.surface;
  }
  // TW2 (2026-11-12, diagnostic with log_diagnostics): how often the memo misses, and why (the epoch, or the slot).
  static uint64_t s_calls = 0, s_epoch = 0, s_slot = 0, s_header = 0;
  if (REXCVAR_GET(log_diagnostics)) {
    ++s_calls;
    if (m.va == surface_va && m.epoch != g_surface_epoch)
      ++s_epoch;
    else if (m.va == surface_va)
      ++s_header;
    else
      ++s_slot;
    if ((s_calls & 0xFFFF) == 0) {
      RGPU_INFO("[surfmemo] 65536 misses: {} by the epoch ({} bumps in all), {} by the header, {} by another VA",
                s_epoch, g_surface_epoch, s_header, s_slot);
      s_epoch = s_slot = s_header = 0;
    }
  }
  HostSurface *result = GetHostSurfaceSlow(surface_va, desc);
  if (memo && result) {
    m.va = surface_va;
    m.size = desc->Size;
    m.info = desc->SurfaceInfo;
    m.format = desc->Format;
    m.color_info = desc->ColorInfo;
    m.epoch = g_surface_epoch;
    m.surface = result;
  }
  return result;
}

HostSurface *GetHostSurfaceSlow(u32 surface_va, const d3d::SurfaceDesc *desc) {
  const bool depth = d3d::IsDepthFormat(desc->Format);
  plume::RenderFormat format = depth ? DepthTargetFormat(desc->Format) : SurfaceColorFormat(*desc);
  // The render resolution, or the finer one graphics_options gives it.
  const RenderScale scale = SurfaceRenderScale(*desc, g_resolution_scale);
  if (depth && tw2de::renut_gpu::Depth24On()) {
    // tw2de (2026-10-19, tw2de_deko3d_depth24): one view, one format (g_depth_view_formats).
    uint8_t &seen = g_depth_view_formats[TargetKey(*desc, true, plume::RenderFormat::UNKNOWN, scale)];
    const uint8_t bit = (desc->Format & 0x3F) == 23 ? 2 : 1;
    if ((seen | bit) == 3 && seen != 3) {
      static uint32_t logged = 0;
      if (logged++ < 8)
        RGPU_WARN("Depth view of surface {:08X} bound as both D24S8 and D24FS8: 32-bit float from now on "
                  "(tw2de_deko3d_depth24)", surface_va);
    }
    seen |= bit;
    if ((seen & 2) && !DepthF24UnormOn())  // TW2 (2026-11-13): then both formats are the 24-bit unorm target
      format = DepthTargetFormat();
  }
  const uint64_t key = TargetKey(*desc, depth, format, scale);
  auto [bound, first_use] = g_surface_keys.try_emplace(surface_va, key);
  if (first_use) {
    UseTarget(key);
    ++g_surface_epoch;
  } else if (bound->second != key) {
    ++g_surface_epoch;
    // The header was rewritten (or the VA reused without Destroy reaching
    // us): the surface is a view of other EDRAM now.
    UnuseTarget(bound->second);
    bound->second = key;
    UseTarget(key);
  }

  auto &slot = g_targets[key];
  if (slot && !depth)
    slot->edram_base = u32(desc->ColorInfo) & 0xFFFu;  // tw2de (2026-10-31): HostSurface::edram_base
  // The view's target covers every surface over it, and every tile while
  // tiling (SetTilingExtentLocked).
  // tw2de (2026-11-08): only a header that can be a tile of it: one that spans the extent's height (columns: a
  // 1600x900 scene's 400x900 tiles, 800x900 with 2x MSAA) or width (rows: a 1280x720 scene's 1280x384 tiles). A
  // 1280x720 header bound while a 1600x900 scene tiles (the HUD's depth, 6D907A60, rewritten between the scene's tile
  // header and its own) was built at 1600x900 at the output's 1.5x (2400x1350) for that frame: the draws over the
  // 1920x1080 picture with it (the save point's glow) were lost.
  const uint32_t header_w = d3d::SurfaceWidth(desc->Size), header_h = d3d::SurfaceHeight(desc->Size);
  const bool tile_header = g_tiling_width && (header_h == g_tiling_height || header_w == g_tiling_width);
  uint32_t min_width = tile_header ? std::max(g_tiling_width, header_w) : header_w;
  uint32_t min_height = tile_header ? std::max(g_tiling_height, header_h) : header_h;
  if (slot && slot->texture) {
    // The surface bound now: its guest size (Targets::guest_width) and D3D
    // format (D24S8 or D24FS8 for a depth resolve).
    slot->size_word = desc->Size;
    slot->info_word = desc->SurfaceInfo;
    slot->format_word = desc->Format;
    if (slot->width >= min_width && slot->height >= min_height && slot->scale == scale)
      return slot.get();
    // A larger surface over the view, or a larger tiling: grow it, keeping
    // the size the others gave it (what it held is lost). (Another scale is
    // another target: TargetKey.)
    min_width = std::max(min_width, slot->width);
    min_height = std::max(min_height, slot->height);
    RetireTarget(*slot);
  } else if (!slot) {
    slot = std::make_unique<HostSurface>();
    slot->guest_va = surface_va;
  }
  ++g_surface_epoch;
  if (g_load_log_active)  // tw2de (2026-11-08, tw2de_diag_load_draw_log)
    RGPU_INFO("[loadlog] target {:08X} {} header {}x{} built at {}x{} code {} drs {} key {:016X} (frame {})", surface_va,
              depth ? "depth" : "colour", d3d::SurfaceWidth(desc->Size), d3d::SurfaceHeight(desc->Size), min_width,
              min_height, scale.code, scale.drs, key, TextureFrameLocked());
  if (TargetLogOn()) {
    ++g_target_log_lines;
    RGPU_INFO("[targets] build {:08X} key {:016X} header {}x{} built {}x{} {} base {:03X} (frame {})", surface_va, key,
              d3d::SurfaceWidth(desc->Size), d3d::SurfaceHeight(desc->Size), min_width, min_height,
              depth ? "depth" : "colour", u32(desc->ColorInfo) & 0xFFFu, TextureFrameLocked());
  }
  if (!BuildTarget(*slot, *desc, min_width, min_height, scale))
    return nullptr;
  slot->out_key = OutputTargetKey(*desc);  // tw2de (2026-11-07)
  // No defined content yet; D3D12 wants a new target discarded (or cleared)
  // before anything else uses it, and a resolve may read it before any draw.
  if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
    TransitionSurfaceLocked(slot.get(), slot->depth ? plume::RenderTextureLayout::DEPTH_WRITE
                                                    : plume::RenderTextureLayout::COLOR_WRITE);
    cmd->discardTexture(slot->texture.get());
  }
  slot->needs_clear = !slot->depth;  // tw2de (2026-10-31): see HostSurface::needs_clear
  if (!slot->depth)
    slot->edram_base = u32(desc->ColorInfo) & 0xFFFu;
  return slot.get();
}

HostSurface *SurfaceByGenerationLocked(uint64_t generation) {
  for (const auto &[key, surface] : g_targets)
    if (surface && surface->texture && surface->generation == generation)
      return surface.get();
  return nullptr;
}

void SurfaceMemoryLocked(size_t &targets, uint64_t &bytes) {
  targets = 0;
  bytes = 0;
  for (const auto &[key, surface] : g_targets) {
    if (!surface || !surface->texture)
      continue;
    ++targets;
    const uint64_t texel = plume::RenderFormatSize(surface->format);
    bytes += uint64_t(surface->scale.Size(surface->width)) * surface->scale.Size(surface->height) *
             texel * surface->samples;
  }
}

void ReleaseHostSurfaceLocked(u32 surface_va) {
  auto it = g_surface_keys.find(surface_va);
  if (it == g_surface_keys.end())
    return;
  const uint64_t key = it->second;
  g_surface_keys.erase(it);
  ++g_surface_epoch;
  UnuseTarget(key);
}

uint32_t SurfaceSampleSlotLocked(HostSurface *surface) {
  if (!surface || !surface->texture)
    return kInvalidDescriptor;
  if (surface->sample_slot != UINT32_MAX)
    return surface->sample_slot;
  // The depth format views the depth plane.
  surface->sample_view = surface->texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(surface->format));
  if (!surface->sample_view)
    return kInvalidDescriptor;
  const uint32_t slot = Video::AllocateTextureSlotLocked();
  if (slot == kInvalidDescriptor)
    return kInvalidDescriptor;
  Video::SetTextureSlotLocked(slot, surface->texture.get(), surface->sample_view.get());
  surface->sample_slot = slot;
  return slot;
}

// tw2de (2026-10-18): the bindless slot of a depth/stencil target's stencil view (read as uint by the depth
// resolve), or kInvalidDescriptor (no stencil, D3D12, or no view). Tried once a surface.
uint32_t SurfaceStencilSlotLocked(HostSurface *surface) {
  if (!surface || !surface->texture || !UsesVulkanBindings() || !plume::RenderFormatIsStencil(surface->format))
    return kInvalidDescriptor;
  if (surface->stencil_slot != UINT32_MAX)
    return surface->stencil_slot;
  surface->stencil_slot = kInvalidDescriptor;
  surface->stencil_view = surface->texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::S8_UINT));
  if (!surface->stencil_view)
    return kInvalidDescriptor;
  const uint32_t slot = Video::AllocateTextureSlotLocked();
  if (slot == kInvalidDescriptor)
    return kInvalidDescriptor;
  Video::SetTextureSlotLocked(slot, surface->texture.get(), surface->stencil_view.get());
  surface->stencil_slot = slot;
  return slot;
}

void TransitionSurfaceLocked(HostSurface *surface, plume::RenderTextureLayout layout) {
  if (!surface || !surface->texture || surface->layout == layout)
    return;
  // tw2de (2026-10-19, tw2de_deko3d_barrier_lean): from one read layout to another: no barrier (textures.cpp).
  if (IsDeko3D() && tw2de::renut_gpu::Deko3DBarrierLeanOn() && ReadOnlyLayout(surface->layout) &&
      ReadOnlyLayout(layout)) {
    surface->layout = layout;
    return;
  }
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  cmd->barriers(layout == plume::RenderTextureLayout::COPY_SOURCE ||
                        layout == plume::RenderTextureLayout::COPY_DEST ||
                        layout == plume::RenderTextureLayout::RESOLVE_SOURCE ||
                        layout == plume::RenderTextureLayout::RESOLVE_DEST
                    ? plume::RenderBarrierStage::COPY
                    : plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(surface->texture.get(), layout));
  surface->layout = layout;
}

void SwapDepthImageLocked(HostSurface &surface, DepthImage &image) {
  ++g_surface_epoch;
  std::swap(surface.texture, image.texture);
  std::swap(surface.layout, image.layout);
  std::swap(surface.generation, image.generation);
  std::swap(surface.sample_view, image.sample_view);
  std::swap(surface.sample_slot, image.sample_slot);
  std::swap(surface.stencil_view, image.stencil_view);
  std::swap(surface.stencil_slot, image.stencil_slot);
  std::swap(surface.handover_views, image.views);
  image.host_width = surface.scale.Size(surface.width);
  image.host_height = surface.scale.Size(surface.height);
  image.format = surface.format;
  image.samples = surface.samples;
  image.format_word = surface.format_word;
  image.colour = !surface.depth;  // TW2 (2026-11-13, tw2de_colour_handover)
}

bool MakeDepthImageLocked(const HostSurface &surface, DepthImage &image) {
  plume::RenderDevice *device = Video::Device();
  const uint32_t w = surface.scale.Size(surface.width), h = surface.scale.Size(surface.height);
  // TW2 (2026-11-13, tw2de_colour_handover): a colour target's image likewise.
  image.colour = !surface.depth;
  image.texture = device->createTexture(
      surface.depth
          ? plume::RenderTextureDesc::DepthTarget(w, h, surface.format, plume::RenderMultisampling(surface.samples))
          : plume::RenderTextureDesc::ColorTarget(w, h, surface.format, plume::RenderMultisampling(surface.samples)));
  if (!TextureCreated(image.texture.get())) {
    image.texture.reset();
    ReportDeviceLost(device);
    return false;
  }
  image.texture->setName(surface.depth ? "renut depth surface (hand-over)" : "renut colour surface (hand-over)");
  image.layout = plume::RenderTextureLayout::UNKNOWN;
  image.generation = g_next_generation++;
  image.host_width = w;
  image.host_height = h;
  image.format = surface.format;
  image.samples = surface.samples;
  image.format_word = surface.format_word;
  return true;
}

void RetireDepthImageLocked(DepthImage &image) {
  // The framebuffers a surface made over this image (cached by its generation).
  for (size_t i = 0; i < g_framebuffers.size();) {
    // TW2 (2026-11-13, tw2de_colour_handover): or as one of its colour targets (generations are unique).
    bool uses = g_framebuffers[i].key.depth && g_framebuffers[i].key.depth_generation == image.generation;
    for (uint32_t c = 0; c < d3d::kMaxRenderTargets; ++c)
      uses = uses || (g_framebuffers[i].key.colors[c] && g_framebuffers[i].key.color_generations[c] == image.generation);
    if (uses) {
      Video::RetireLocked(std::move(g_framebuffers[i].framebuffer));
      g_framebuffers[i] = std::move(g_framebuffers.back());
      g_framebuffers.pop_back();
      ++g_framebuffer_epoch;
    } else {
      ++i;
    }
  }
  FreeDepthViews(image.views);
  if (image.sample_slot != UINT32_MAX && image.sample_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(image.sample_slot);
  image.sample_slot = UINT32_MAX;
  Video::RetireLocked(std::move(image.sample_view));
  if (image.stencil_slot != UINT32_MAX && image.stencil_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(image.stencil_slot);
  image.stencil_slot = UINT32_MAX;
  Video::RetireLocked(std::move(image.stencil_view));
  Video::RetireLocked(std::move(image.texture));
}

void TransitionDepthImageLocked(DepthImage &image, plume::RenderTextureLayout layout) {
  if (!image.texture || image.layout == layout)
    return;
  if (IsDeko3D() && tw2de::renut_gpu::Deko3DBarrierLeanOn() && ReadOnlyLayout(image.layout) && ReadOnlyLayout(layout)) {
    image.layout = layout;
    return;
  }
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image.texture.get(), layout));
  image.layout = layout;
}

uint32_t DepthImageViewSlotLocked(plume::RenderTexture *texture, plume::RenderFormat format,
                                  std::vector<DepthImage::View> &views, const plume::RenderComponentMapping &mapping,
                                  uint32_t rows) {
  if (!texture)
    return kInvalidDescriptor;
  for (const DepthImage::View &v : views) {
    if (v.rows == rows && v.mapping.r == mapping.r && v.mapping.g == mapping.g && v.mapping.b == mapping.b &&
        v.mapping.a == mapping.a)
      return v.slot;
  }
  // The depth format views the depth plane (deko3d samples it in the red channel), with the destination's mapping.
  plume::RenderTextureViewDesc desc = plume::RenderTextureViewDesc::Texture2D(format);
  desc.componentMapping = mapping;
  DepthImage::View v;
  v.mapping = mapping;
  v.rows = rows;
  plume::RenderTexture *viewed = texture;
  if (rows) {
    // TW2 (2026-11-13, tw2de_colour_handover_rows): the image's first rows as a texture of their own over its memory.
#if defined(PLUME_DEKO3D)
    if (IsDeko3D())
      v.rows_texture = plume::Deko3DCreateTextureRows(texture, rows);
#endif
    if (!v.rows_texture)
      return kInvalidDescriptor;
    viewed = v.rows_texture.get();
  }
  v.view = viewed->createTextureView(desc);
  if (!v.view) {
    if (v.rows_texture)
      Video::RetireLocked(std::move(v.rows_texture));
    return kInvalidDescriptor;
  }
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    if (v.rows_texture)
      Video::RetireLocked(std::move(v.rows_texture));
    return kInvalidDescriptor;
  }
  Video::SetTextureSlotLocked(v.slot, viewed, v.view.get());
  views.push_back(std::move(v));
  return views.back().slot;
}

plume::RenderFramebuffer *GetFramebufferLocked(HostSurface *const *colors, uint32_t color_count,
                                               HostSurface *depth) {
  FramebufferKey key;
  for (uint32_t i = 0; i < color_count && i < d3d::kMaxRenderTargets; ++i) {
    key.colors[i] = colors[i];
    key.color_generations[i] = colors[i] ? colors[i]->generation : 0;
  }
  key.depth = depth;
  key.depth_generation = depth ? depth->generation : 0;
  // tw2de (2026-10-19, render-thread proposal 8): the recent ones first, the found one moved to the front.
  for (uint32_t i = 0; i < 4; ++i) {
    RecentFramebuffer &r = g_recent_framebuffers[i];
    if (r.epoch == g_framebuffer_epoch && r.framebuffer && r.key == key) {
      plume::RenderFramebuffer *found = r.framebuffer;
      if (i) {
        const RecentFramebuffer hit = r;
        for (uint32_t j = i; j > 0; --j)
          g_recent_framebuffers[j] = g_recent_framebuffers[j - 1];
        g_recent_framebuffers[0] = hit;
      }
      return found;
    }
  }
  auto remember = [&key](plume::RenderFramebuffer *framebuffer) {
    for (uint32_t j = 3; j > 0; --j)
      g_recent_framebuffers[j] = g_recent_framebuffers[j - 1];
    g_recent_framebuffers[0] = {key, framebuffer, g_framebuffer_epoch};
    return framebuffer;
  };
  for (auto &cached : g_framebuffers) {
    if (cached.key == key)
      return remember(cached.framebuffer.get());
  }

  const plume::RenderTexture *attachments[d3d::kMaxRenderTargets] = {};
  uint32_t count = 0;
  for (uint32_t i = 0; i < color_count && i < d3d::kMaxRenderTargets; ++i) {
    if (!colors[i] || !colors[i]->texture)
      break;  // host framebuffers need contiguous color attachments
    attachments[count++] = colors[i]->texture.get();
  }
  plume::RenderFramebufferDesc desc(attachments, count,
                                    depth && depth->texture ? depth->texture.get() : nullptr);
  auto framebuffer = Video::Device()->createFramebuffer(desc);
  if (!framebuffer) {
    RGPU_ERROR("Framebuffer creation failed ({} color targets, depth {})", count, depth != nullptr);
    return nullptr;
  }
  g_framebuffers.push_back({key, std::move(framebuffer)});
  return remember(g_framebuffers.back().framebuffer.get());
}

} // namespace renut::gpu

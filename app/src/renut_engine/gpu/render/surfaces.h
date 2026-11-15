/**
 * @file    gpu/render/surfaces.h
 * @brief   Host render targets behind guest D3DSurfaces.
 *
 * N&B's own D3DDevice_CreateSurface still builds the surface (and does its
 * EDRAM bookkeeping); the renderer attaches a host color or depth target to
 * the EDRAM view the surface describes (base, pitch, samples, format) the
 * first time a surface over it is bound, sized and formatted from the surface
 * header. Surfaces over the same view share the target, as they share EDRAM
 * on the GPU; content lives in it while any of them does, which is what the
 * game relies on between Clear, draws and Resolve.
 * All functions require State().mutex.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include <rex/types.h>

#include "renut_engine/gpu/render/render_scale.h"

namespace renut::gpu {

// tw2de (2026-10-19, tw2de_deko3d_depth_handover): a depth target's image and what is made over it (its generation,
// layout, sample and stencil views), which moves between a surface and a depth resolve destination that holds the
// surface's content (render/draw.cpp, "Depth hand-over"). 'views' are a destination's views of it, by component
// mapping (the fetch swizzle as the destination's own R32 views take it).
struct DepthImage {
  std::unique_ptr<plume::RenderTexture> texture;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint64_t generation = 0;
  std::unique_ptr<plume::RenderTextureView> sample_view;
  uint32_t sample_slot = UINT32_MAX;
  std::unique_ptr<plume::RenderTextureView> stencil_view;
  uint32_t stencil_slot = UINT32_MAX;
  struct View {
    plume::RenderComponentMapping mapping;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t slot = UINT32_MAX;
    // TW2 (2026-11-13, tw2de_colour_handover_rows): a view of the image's first 'rows' rows only (0: of all of it),
    // through a texture of that many rows laid over the image's memory (plume::Deko3DCreateTextureRows).
    uint32_t rows = 0;
    std::unique_ptr<plume::RenderTexture> rows_texture;
  };
  std::vector<View> views;
  // The target it was made for or last taken from: a surface takes it back only when these are its own.
  uint32_t host_width = 0, host_height = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderSampleCounts samples = plume::RenderSampleCount::COUNT_1;
  u32 format_word = 0;
  // TW2 (2026-11-13, tw2de_colour_handover): a colour target's image (render/draw.cpp, "Colour hand-over").
  bool colour = false;
};

struct HostSurface {
  u32 guest_va = 0;  // the first surface over the view
  // tw2de (2026-11-07): OutputTargetKey of its view (graphics_options.h): which 1280x720 targets of a 1280x720
  // scene are drawn at 1.5x for a 1920x1080 output.
  uint64_t out_key = 0;
  // Header words of the surface over the view bound most recently.
  u32 size_word = 0;
  u32 info_word = 0;
  u32 format_word = 0;

  // In guest pixels; the host target is 'scale' times that on each axis
  // (ResolutionScale when it was built).
  uint32_t width = 0;
  uint32_t height = 0;
  RenderScale scale;
  bool depth = false;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderSampleCounts samples = plume::RenderSampleCount::COUNT_1;

  std::unique_ptr<plume::RenderTexture> texture;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint64_t generation = 0;  // bumped on rebuild, part of framebuffer keys
  // tw2de (2026-10-19, tw2de_renut_resolve_skip): bumped by every draw that can write it (a colour write mask; for
  // depth, a depth write or stencil writes) and every clear of it; part of a resolve's signature.
  uint64_t writes = 0;
  // TW2 (2026-11-14, tw2de_depth_handover_readonly): those of 'writes' that were draws which cannot change a depth
  // target's depth (stencil only, or a depth test that never passes): writes - stencil_only_writes stamps its depth.
  uint64_t stencil_only_writes = 0;
  // tw2de (2026-10-31): a colour target built since it was last bound, its memory undefined: cleared to transparent
  // black when first bound (draw.cpp BindTargets). The game clears the scene's depth but not its colour, and leaves the
  // pixels no geometry reaches (the far ones, which its fog pass then covers) as they were; the console's EDRAM held
  // defined data there, a new host image whatever its memory last held (FINAL FANTASY XIII, behind the crystal in the
  // Vestige: bright noise the bloom and light shafts spread into a flickering glow).
  bool needs_clear = false;
  // TW2 (2026-11-13, tw2de_diag_colour_handover_poison): that clear in magenta (an image the colour hand-over gave
  // the target in place of the one a resolve destination took: its old content must not show).
  bool poison = false;
  // tw2de (2026-10-31, the EDRAM a colour target shares with others): its EDRAM base (ColorInfo's low 12 bits; colour
  // targets only), the frame it was last drawn into and cleared in, and the order of its last write (draw.cpp
  // EdramCarryLocked).
  u32 edram_base = 0xFFFFFFFFu;
  uint64_t drawn_frame = UINT64_MAX, cleared_frame = UINT64_MAX;
  uint64_t write_order = 0;

  // The view and bindless slot shaders read the target through (depth
  // resolves); made on first use, see SurfaceSampleSlotLocked.
  std::unique_ptr<plume::RenderTextureView> sample_view;
  uint32_t sample_slot = UINT32_MAX;
  // tw2de (2026-10-18): a depth/stencil target's stencil view and its slot (SurfaceStencilSlotLocked).
  std::unique_ptr<plume::RenderTextureView> stencil_view;
  uint32_t stencil_slot = UINT32_MAX;
  // tw2de (2026-10-19, tw2de_deko3d_depth_handover): the current image's views for destinations that sample it
  // (DepthImage::views; they go with the image), how many destinations hold its content, and whether anything has
  // used its stencil (a draw with the stencil test, a stencil clear): a clear of the depth alone hands the image over
  // only when nothing has. A stencil nothing writes is undefined either way (a depth resolve's words read it as is).
  std::vector<DepthImage::View> handover_views;
  uint32_t handed = 0;
  bool stencil_used = false;

  // gpu_profile labels of draws with it as the first target: color only,
  // color with depth, depth only. Made on first use.
  uint32_t profile_labels[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
  // tw2de (2026-10-25): gpu_profile labels of its resolves: 0-3 a resolve (bit 0 to a texture, bit 1 clear after),
  // 4-5 a depth resolve to a texture (4 with the guest words, 5 the depth only). Made on first use.
  uint32_t resolve_labels[6] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
};

// Render resolution: every host render target, and every texture a resolve
// writes, is this many times its guest size on each axis (1 = the game's own
// 720p, 3 = 2160p, 2/3 = 480p). The renderer works in guest pixels and scales
// where it records host viewports, scissors, clears and copies; shaders see a
// scaled texture's guest size (the scale is in its descriptor index, see
// kResolutionScaleShift) and the guest pixel position.
// From the renut_resolution cvar, latched once per frame by
// LatchResolutionScaleLocked (at Swap) so a frame never mixes scales. Targets
// and resolve destinations of another scale are rebuilt on their next use.
RenderScale ResolutionScale();
void LatchResolutionScaleLocked();

// The host target for the D3DSurface at 'surface_va', created or rebuilt as
// needed. nullptr for 0 or when the target cannot be created.
HostSurface *GetHostSurfaceLocked(u32 surface_va);

// tw2de: the bound surfaces' headers as they were when the game queued the draw, clear or resolve. The game
// rewrites the headers of a few D3DSurface objects between passes (one object is a 1280x720 colour target, then a
// 1024x576 one, then the depth buffer), and the render thread runs frames behind: it read the header the game had
// written since, and drew into, resolved from and cleared the wrong targets. While set (render thread), a VA here
// is described by its snapshot instead of guest memory.
struct SurfaceSnapshot {
  u32 va[5] = {};          // render targets 0-3, depth stencil
  u32 desc[5][6] = {};     // their d3d::SurfaceDesc words (big-endian, as in guest memory)
  bool valid[5] = {};
};
void SnapSurfaces(const u8 *device_bytes, SurfaceSnapshot &out);  // game thread
void SetSurfaceSnapshotLocked(const SurfaceSnapshot *snapshot);  // render thread; nullptr: guest memory again
namespace d3d { struct SurfaceDesc; }
const d3d::SurfaceDesc *SurfaceDescLocked(u32 surface_va);  // the snapshot's, else guest memory's
// tw2de (2026-10-20, from sa2-nx, tw2de_renut_chain_reuse): tw2de's surface epoch (bumped by every target built, retired,
// unused or rebound, the resolution scale latched and the tiling extent set).
uint64_t SurfaceEpochLocked();
// tw2de (2026-11-07): every surface looked up again (a target's scale changed: graphics_options.h).
void BumpSurfaceEpochLocked();
// tw2de (2026-10-20, from sa2-nx): tw2de's framebuffer epoch (bumped when a framebuffer is retired).
uint64_t FramebufferEpochLocked();

// Only if one exists already.
HostSurface *FindHostSurfaceLocked(u32 surface_va);
// tw2de (2026-10-24): kept unused targets past their time released (once a frame).
void SweepUnusedTargetsLocked();

// The other color targets over the same EDRAM view as 'surface' (base tile,
// pitch, samples and scale), in another host format: on the console one
// memory, so what a clear writes through one is what the others then hold.
void ColorAliasesLocked(const HostSurface *surface, std::vector<HostSurface *> &out);

// Predicated tiling (D3DDevice_BeginTiling .. EndTiling): the game draws the
// frame into surfaces the size of one EDRAM tile, and the GPU replays the
// draws once per tile with the tile's screen offset, resolving each tile into
// its part of the destination. The host has no EDRAM to fit, so a surface
// used while tiling is backed by a target covering every tile (and keeps that
// size), which the draws and the per-tile resolves address in screen space.
// 'width' x 'height' is the bottom-right corner of the tile rects; 0 x 0 ends
// tiling.
void SetTilingExtentLocked(uint32_t width, uint32_t height);
// tw2de (2026-11-08): the extent set (0 x 0 when not tiling).
void TilingExtentLocked(uint32_t &width, uint32_t &height);

// The surface no longer uses its target (guest D3DResource_Destroy); the
// target is retired when no surface over its view remains.
void ReleaseHostSurfaceLocked(u32 surface_va);

// Bindless slot of the surface's target (the depth plane for depth targets,
// multisampled when the target is), or kInvalidDescriptor.
uint32_t SurfaceSampleSlotLocked(HostSurface *surface);
// tw2de (2026-11-06): the guest's sample count of a surface info word (1, 2 or 4); the host's may be more
// (tw2de_msaa_samples, surfaces.cpp).
uint32_t GuestSampleCount(u32 info_word);
// tw2de (2026-10-18): its stencil view's slot (uint), or kInvalidDescriptor.
uint32_t SurfaceStencilSlotLocked(HostSurface *surface);

// Moves 'surface' into 'layout' with a barrier on the open command list.
void TransitionSurfaceLocked(HostSurface *surface, plume::RenderTextureLayout layout);

// For the memory report: host render targets and the video memory they take
// (estimated from format, size and samples).
void SurfaceMemoryLocked(size_t &targets, uint64_t &bytes);

// tw2de (2026-10-19): the live surface with this generation, or nullptr (rebuilt or gone since).
HostSurface *SurfaceByGenerationLocked(uint64_t generation);

// tw2de (2026-10-19, tw2de_deko3d_depth_handover): exchanges the surface's image (its texture, generation, layout
// and views) with 'image'. Framebuffers are cached by surface and generation, so each image keeps its own.
void SwapDepthImageLocked(HostSurface &surface, DepthImage &image);
// A new image of the surface's size, format and samples for SwapDepthImageLocked; false when it cannot be made.
bool MakeDepthImageLocked(const HostSurface &surface, DepthImage &image);
// Retires the image, its views and the framebuffers made over it (deferred past in-flight frames).
void RetireDepthImageLocked(DepthImage &image);
// Moves the image into 'layout' with a barrier on the open command list.
void TransitionDepthImageLocked(DepthImage &image, plume::RenderTextureLayout layout);
// The slot of a view of the image with 'mapping' (made on first use, kept in 'views'), or kInvalidDescriptor.
// TW2 (2026-11-13): 'rows' not 0: of its first 'rows' rows only (DepthImage::View::rows).
uint32_t DepthImageViewSlotLocked(plume::RenderTexture *texture, plume::RenderFormat format,
                                  std::vector<DepthImage::View> &views, const plume::RenderComponentMapping &mapping,
                                  uint32_t rows = 0);
// render/draw.cpp: a depth target is about to go; the destinations that still hold its content get it first.
void DepthHandOverBeforeRetireLocked(HostSurface *surface);

// Framebuffer over up to 4 color targets plus depth; cached per target set.
plume::RenderFramebuffer *GetFramebufferLocked(HostSurface *const *colors, uint32_t color_count,
                                               HostSurface *depth);

} // namespace renut::gpu

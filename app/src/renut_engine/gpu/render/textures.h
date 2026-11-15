/**
 * @file    gpu/render/textures.h
 * @brief   Host textures behind guest D3DBaseTextures, and host samplers.
 *
 * A texture is identified by its D3DBaseTexture VA (created or embedded in a
 * resource bundle) and described by the fetch constant in its own header.
 * The host copy is built from guest memory the first time the texture is
 * bound, untiled and converted, and rebuilt when the header describes other
 * data or a sampled content check sees the bytes change. Textures a Resolve
 * wrote into keep the GPU copy instead. All functions require State().mutex.
 */
#pragma once

#include <cstddef>
#include <functional>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/render_scale.h"
#include "renut_engine/gpu/render/texture_layout.h"

namespace renut::gpu {

// tw2de (2026-10-28): the render thread's time uploading textures since the last call, in ns (draw.cpp).
uint64_t TakeTextureUploadNs();
// tw2de (2026-10-28): the uploads since the last call, as one line (empty: none).
std::string TakeTextureUploadReport();

struct DepthImage;   // render/surfaces.h
struct HostSurface;

struct HostTexture {
  u32 guest_va = 0;
  u32 desc[6] = {};  // the header fetch constant bits that define the data

  TextureDimension dimension = TextureDimension::Tex2D;
  GpuTextureFormat guest_format = GpuTextureFormat::Fmt8_8_8_8;
  TextureFormatInfo info;
  uint32_t width = 0;       // guest size
  uint32_t height = 0;
  uint32_t depth = 1;       // 3D depth, or array slices (6 for cubes)
  // Resolve destinations (and copies of a depth resolve's words): the host
  // texture is 'scale' times the guest width and height, the render
  // resolution of the targets resolved into it (ResolutionScale).
  RenderScale scale;
  // tw2de (2026-11-07, dynamic resolution): the level its last resolve was drawn at (scale.drs; dynamic_resolution.h
  // kDrsCodeShift: 0 none), which its bindings carry (TextureBinding::drs_code).
  uint32_t drs_code = 0;
  uint32_t mip_levels = 1;
  // Host levels past the guest's one, for a scaled 2D color resolve
  // destination (UpdateResolveMipsLocked): its guest texel spans 'scale' host
  // texels, and these levels let a pass that samples it smaller (the game's
  // downsamples to quarter size and below) average what the guest texels
  // cover, as one guest bilinear tap does. Sampled up to LOD log2(scale).
  uint32_t extra_levels = 0;
  // Level 0 without the fetch swizzle, which the levels are made from.
  std::unique_ptr<plume::RenderTextureView> level0_view;
  uint32_t level0_slot = UINT32_MAX;
  // A stacked 2D texture: a host 2D array (every layer in every mip), though
  // the shaders fetch it with 3D fetches (dimension k3D).
  bool stacked = false;

  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  plume::RenderFormat host_format = plume::RenderFormat::UNKNOWN;
  uint32_t alloc_width = 0;   // host texture size (block-aligned for BC, scaled)
  uint32_t alloc_height = 0;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint32_t slot = UINT32_MAX;  // bindless texture slot
  bool render_target = false;  // created as a color target (resolve destination)
  // What 'view' was made with: the view and the fetch constant swizzle it
  // applies (X and Z exchanged for content stored so). Another fetch constant
  // over the same data may swizzle it otherwise (a depth resolve sampled for
  // its depth in W), and gets a view of its own (SlotForSwizzle).
  plume::RenderTextureViewDesc view_desc;
  u32 view_swizzle = 0;
  bool view_swap_red_blue = false;
  struct SwizzleView {
    u32 swizzle = 0;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t slot = UINT32_MAX;
  };
  std::vector<SwizzleView> swizzle_views;

  bool gpu_written = false;     // content comes from a Resolve, not guest memory
  // tw2de (2026-10-19, render-thread proposal 1, tw2de_renut_store_cards): the last content check's hash and the
  // card interval it was taken in (a check while no page of the texture was written since reuses it), and the
  // interval its last deferred upload read the guest bytes in (a dynamic texture is not uploaded again while none
  // was written since).
  uint64_t last_hash = 0;
  uint64_t hash_interval = UINT64_MAX;
  uint64_t read_interval = UINT64_MAX;
  uint64_t trace_frame = UINT64_MAX;  // tw2de: tw2de_renut_diag_resolve_trace's frame
  bool uploaded = false;
  uint64_t content_hash = 0;
  uint64_t checked_frame = 0;
  // The last frame it was bound or resolved into (EvictIdleTexturesLocked).
  uint64_t used_frame = 0;
  // Linear (CPU-written) textures, see BindTextureLocked:
  bool unlocked = false;       // D3D::UnlockResource since the last check
  uint64_t unlock_frame = 0;   // frame of the last unlock
  bool lock_driven = false;    // written between Lock and Unlock (the font cache)
  bool dynamic = false;        // changes all the time (movie frames)
  uint32_t changes = 0;        // checks in a row that saw the content change
  uint64_t verified_frame = 0; // last periodic full check
  // What SampleHash / ContentHash read: the base level's and the mip chain's
  // first page and extent in bytes, fixed by desc, worked out at the first check.
  bool sample_known = false;
  uint32_t sample_pages[2] = {};
  uint32_t sample_sizes[2] = {};

  // Depth resolve destinations: the guest words the resolve wrote (see
  // shaders/host/depth_resolve.hlsli) and the framebuffer over both targets.
  // 'resolves' counts the resolves into it.
  std::unique_ptr<HostTexture> words;  // a bare target: texture and layout
  std::unique_ptr<plume::RenderFramebuffer> depth_framebuffer;
  // tw2de (2026-10-18): a resolve destination as the target of DrawResolveCopy (draw.cpp), made at its first one.
  std::unique_ptr<plume::RenderFramebuffer> copy_framebuffer;
  // tw2de (2026-11-12): DrawResolveCopy into one layer of a cube map (the point lights' shadow faces): a 2D view of
  // each layer and its framebuffer, made at its first resolve.
  struct SliceTarget {
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  };
  std::vector<SliceTarget> slice_targets;
  // tw2de (2026-11-08): its host levels as targets of their own (UpdateResolveMipsLocked draws each level straight
  // into the texture on deko3d: no scratch target and no copy, whose full barriers made the levels of the 1920x1080
  // post-processing textures cost ~0.65 ms each), with a sampled view of each (the next level's source).
  struct MipTarget {
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    uint32_t slot = UINT32_MAX;
  };
  std::vector<MipTarget> mip_targets;  // index = level (0 unused)
  // tw2de (2026-11-08, tw2de_lazy_resolve_mips): the region of level 0 resolved since its host levels were last
  // made: they are made when a draw samples it into a smaller target (MakePendingMipsLocked), not at every resolve.
  plume::RenderRect mips_pending{};
  bool mips_pending_set = false;
  uint64_t resolves = 0;
  // tw2de (2026-10-19, tw2de_renut_resolve_skip): the signature of the last resolve into it (render/draw.cpp), 0 once
  // anything else wrote it (an upload, a words copy).
  uint64_t resolve_signature = 0;
  // tw2de (2026-10-19, tw2de_renut_depth_words_lazy): a depth resolve destination writes its words (the 8_8_8_8
  // view of its memory) only once something has read them, or in its first resolves (words_probe counts them);
  // words_current: the words are the last resolve's. The last resolve's source (its surface by generation, the writes
  // it had) and place, to write the words late from an unchanged source (EnsureDepthWordsLocked).
  bool words_wanted = false;
  bool words_current = true;
  uint32_t words_probe = 0;
  // TW2 (2026-11-13): resolves into it since the words were last asked for (EnsureDepthWordsLocked): after
  // kDepthWordsProbe of them nothing reads the words any more (TW2: only the title's half-size depth copy
  // did) and they are not written until something asks again.
  uint32_t words_idle = 0;
  uint64_t last_source_generation = 0;
  uint64_t last_source_writes = 0;
  int32_t last_src[4] = {};
  int32_t last_dest_x = 0, last_dest_y = 0;
  uint32_t last_sample = 0;  // tw2de: the last resolve's sample (D3DRESOLVE_FRAGMENTn)
  // A texture over a depth resolve's memory: the resolve its words were
  // copied from last.
  uint64_t copied_resolve = 0;
  // tw2de: a texture over the memory of several resolve destinations (the game's two 1024x1024 shadow cascades
  // resolved one after the other and sampled as one 1024x2048 texture): what was copied from them last (the sum of
  // their resolve counts and the frame), ResolveComposite.
  uint64_t composite_copied = ~0ull;
  // tw2de (2026-11-05): its last upload made every level with pending memexport results from their windows on the
  // GPU (textures.cpp Upload): once the results are in guest memory, their hash is taken as the content's, not uploaded.
  bool export_adopt = false;
  uint64_t full_hash = 0;  // tw2de (tw2de_texture_full_hash)
  // tw2de (tw2de_gpu_depth_words_view, package 28): views of 'words' that the textures over the resolve's memory
  // sample instead of a copy (one per fetch swizzle), with their bindless slots; they go with 'words'.
  struct WordsView {
    u32 swizzle = 0;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t slot = UINT32_MAX;
  };
  std::vector<WordsView> words_views;
  // tw2de (2026-10-19, tw2de_deko3d_depth_handover; render/draw.cpp, "Depth hand-over"): a depth resolve
  // destination whose content is not in 'texture' but in the image of the target it was resolved from (handed_from,
  // which has its generation and writes since), or in 'held' (that image, taken at the target's next whole clear;
  // held_current). 'held' stays as the image the target takes at the clear after.
  HostSurface *handed_from = nullptr;
  uint64_t handed_generation = 0;
  uint64_t handed_writes = 0;
  // TW2 (2026-11-14, tw2de_lookahead; render/draw.cpp "Look-ahead").
  bool la_dead = false;   // its content was given up: the next item to touch it is a resolve over all of it
  bool la_watch = false;  // its content was copied with the queue not telling: which comes first, a use or a resolve
  bool la_hint = false;   // last time a resolve came first, close by: the queue is worth a short wait
  uint8_t la_timeouts = 0;
  uint64_t la_watch_serial = 0;
  std::shared_ptr<DepthImage> held;
  bool held_current = false;
};

struct TextureBinding {
  uint32_t slot = 0;
  TextureDimension dimension = TextureDimension::Tex2D;
  bool stacked = false;  // a 2D array fetched as 3D (dimension Tex3D)
  RenderScale scale;     // HostTexture::scale
  // The scale its host levels reach (HostTexture::extra_levels), 1x without
  // them; the sampler takes it (SamplerSlotLocked).
  RenderScale lod_scale;
  uint32_t drs_code = 0;  // tw2de (2026-11-07): HostTexture::drs_code
};

// Flags on a stacked texture's 3D descriptor index (RENUT_STACKED_TEXTURE,
// RENUT_STACKED_LAYER_LINEAR and RENUT_STACKED_LAYER_WRAP in
// shader_common.h). The layer wraps when the fetch constant's Z address mode
// does: the host sampler's W mode does not reach an array index.
constexpr uint32_t kStackedTextureBit = 0x80000000u;
constexpr uint32_t kStackedLayerLinearBit = 0x40000000u;
constexpr uint32_t kStackedLayerWrapBit = 0x20000000u;
// Any descriptor index: bits 26-28 hold HostTexture::scale's code, whose
// factor the shaders divide the texture's size by for texel offsets and
// texel coordinates (RENUT_RESOLUTION_SCALE_SHIFT in shader_common.h).
constexpr uint32_t kResolutionScaleShift = 26;
// TW2 (2026-11-12): the fetch constant's exponent adjust in a texture word, bits 16-19 (signed; shader_common_glsl.h
// renutExpScale). The descriptor index takes bits 0-15.
constexpr uint32_t kExpAdjustShift = 16;
// The most host levels a resolve destination gets (ExtraLevels).
constexpr uint32_t kMaxExtraLevels = 3;

// Starts a new frame for content checks.
void AdvanceTextureFrameLocked();
// tw2de (tw2de_renut_diag_draw_log_s): the frame number AdvanceTextureFrameLocked counts.
// tw2de (2026-10-19, render-thread proposal 22): inline (called per fetch slot).
extern uint64_t g_texture_frame;
inline uint64_t TextureFrameLocked() { return g_texture_frame; }

// D3D::UnlockResource: the CPU has written the resource at 'resource_va'. Any
// thread; does not need State().mutex.
void NoteTextureUnlocked(u32 resource_va);

// Reads the guest data of uploads recorded with a deferred read (linear
// textures) into their staging memory. Before the command list is submitted.
void FlushTextureFillsLocked();

// tw2de (2026-11-05, seen15/16): runs fn(0) on the calling thread and fn(1), fn(2) on the two copy helpers (cores 0
// and 2, the render thread's priority; the large texture untiles' helpers) as they get
// to it, and returns once fn(0) is done and the helpers that joined are. fn must take its work from a shared counter
// until there is none (a helper may join late or not at all; fn(0) alone must be able to do it all). The render thread
// only (the helpers serve one caller at a time).
constexpr u32 kCopyHelperParts = 3;
void RunOnCopyHelpers(const std::function<void(u32)> &fn);
// A memcpy split over the same three threads from kParallelMemcpyBytes up (one core's memcpy into the upload ring ran at
// about 2.7 GB/s: 39 MB of texture levels for the GPU untile took 14.4 ms at an area entry, run d484a129), plain below.
// Render thread only. The two ranges must not overlap (memcpy's rule).
constexpr size_t kParallelMemcpyBytes = 256u << 10;
void ParallelMemcpy(void *dst, const void *src, size_t bytes);

// Bindless slot for the texture bound as 'texture_va'. Falls back to the null
// texture of the fetch constant's dimension when there is nothing to bind.
// tw2de (2026-10-19, CPU proposal 2): what a bind did besides its result, for FillTextures' memo to do again on a
// hit: the textures it moved to SHADER_READ and its counters.
struct BindEffects {
  HostTexture *transition[2] = {nullptr, nullptr};
  // tw2de (2026-10-19, tw2de_deko3d_depth_handover): the target image a destination was sampled through.
  HostSurface *handover_surface = nullptr;
  DepthImage *handover_image = nullptr;
  bool float_sampled = false;  // CountDepthFloatSampled
  bool words_viewed = false;   // CountDepthWords(true)
  // tw2de (2026-10-19, render-thread proposal 3): the bind went through a depth resolve's words (a view of them, or
  // a copy; EnsureDepthWordsLocked), which every depth resolve can change: such a memo entry also holds g_depth_epoch.
  bool depth_source = false;
};
// Bumped by everything BindTextureLocked reads besides its arguments and the guest header: a texture created, retired
// or uploaded, a resolve destination set (every resolve), depth words copied or a view of them made.
extern uint64_t g_texture_epoch;
// tw2de (2026-10-19, render-thread proposal 3): bumped by every depth resolve (GetDepthResolveTextureLocked).
// g_texture_epoch is now bumped only by what changes a binding: a texture made or retired, a view or slot made, a
// resolve destination or depth words entry that changes, depth words copied, a depth hand-over's state; no longer by
// texture uploads (content) or by resolves into the destination already there.
extern uint64_t g_depth_epoch;
TextureBinding BindTextureLocked(u32 texture_va, const TextureFetch &device_fetch, BindEffects *fx = nullptr);

// tw2de (2026-10-19, tw2de_deko3d_depth_handover; render/draw.cpp): the slot a depth resolve destination is sampled
// through while its content is a target's image ('mapping': the fetch swizzle as its own views take it), or UINT32_MAX
// (its own texture); and a destination that goes stops holding anything.
uint32_t DepthHandOverSlotLocked(HostTexture &dest, const plume::RenderComponentMapping &mapping, BindEffects *fx);
void DepthHandOverForgetLocked(HostTexture &dest);
// tw2de (2026-11-08): a destination left in a target's image drawn into its own texture (for a copy of it).
// 0 nothing to draw, 1 drawn from a target's image, 2 from a held image, 3 / 4 the same failed.
int DepthHandOverMaterialiseLocked(HostTexture &dest);
// TW2 (2026-11-14, tw2de_lookahead): a bind of a destination with la_dead or la_watch set (render/draw.cpp), and
// whether six fetch words (TextureFetch::FromHeader of a resolve's destination header) name 't's data.
void LookAheadUsedLocked(HostTexture &t);
bool SameHostTextureDataLocked(const HostTexture &t, const u32 dwords[6]);

// The guest memory a texture's data spans: pages[0]/sizes[0] its base level,
// pages[1]/sizes[1] its mip chain (a page is 4 KiB of physical memory; a zero
// page or size is absent).
void TextureGuestExtent(const TextureFetch &fetch, uint32_t pages[2], uint32_t sizes[2]);

// Host texture for a resolve into the guest texture, created as a color
// target in 'format' when there is none yet (or the existing one does not
// match, or is of another 'scale': the source target's). Marks it
// GPU-written. 'header_format', when given, is the header's fetch constant as
// it was when the resolve was recorded (the header may be a temporary the
// game has since overwritten).
HostTexture *GetResolveTextureLocked(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                                     const d3d::TextureFetchConstant *header_format = nullptr);

// The same for a depth resolve: a kDepthResolveFormat target plus the guest
// words (kDepthResolveWordsFormat), which a texture of a 32-bit 8_8_8_8
// format over the same memory is given (N&B's depth of field reads the depth
// through such a header). Bumps 'resolves'.
HostTexture *GetDepthResolveTextureLocked(u32 texture_va, RenderScale scale,
                                          const d3d::TextureFetchConstant *header_format = nullptr);

// Existing host texture for a guest texture VA, or nullptr.
HostTexture *FindHostTextureLocked(u32 texture_va);
// tw2de (tw2de_frame_dump): reads the texture a draw samples back into <dir>/tex_<va>.bin. Submits and waits.
uint64_t TextureFrameForLog();  // tw2de: diagnostics
void DumpTextureLocked(u32 texture_va, const TextureFetch &fetch, const std::string &dir, int draw = -1);
// tw2de (2026-10-31): bytes a block of a host format (and its block size in pixels); 0 when the dump does not read it.
uint32_t DumpBlockBytes(plume::RenderFormat f, uint32_t &block);
// tw2de (2026-10-22, tw2de_frame_dump): what the host texture DumpTextureLocked would read holds now (it changes
// with every resolve into it); 0: none.
uint64_t DumpContentSignatureLocked(u32 texture_va, const TextureFetch &fetch);

void ReleaseHostTextureLocked(u32 texture_va);

// The game freed the guest memory at physical address 'physical' ('size'
// bytes): the host textures whose data was there go. N&B's level textures are
// headers it sets up itself (XGSetTextureHeader, sub_823191B8 and the
// streaming's sub_8230B360) over data in its section pools; D3D never destroys
// them, and the game drops them by destroying the pool at a section change
// (game/level_pools.cpp).
void ReleaseHostTexturesInMemoryLocked(u32 physical, u32 size);

void TransitionTextureLocked(HostTexture *texture, plume::RenderTextureLayout layout);
// tw2de (2026-10-19): a layout a texture is only read in (tw2de_deko3d_barrier_lean).
bool ReadOnlyLayout(plume::RenderTextureLayout layout);

// Rebuilds the host levels of a resolve destination (HostTexture::extra_levels)
// over 'rect' of level 0, in host texels, after a resolve wrote it. Leaves the
// texture in SHADER_READ and the command list's framebuffer, pipeline and
// viewport changed.
void UpdateResolveMipsLocked(HostTexture *texture, const plume::RenderRect &rect);
// tw2de (2026-11-08, tw2de_lazy_resolve_mips): textures whose host levels wait for a draw that samples them
// smaller; MakePendingMipsLocked makes those of the texture at 'texture_va' when 'target_width' (host pixels) is under
// its width (true: made: the caller's targets, pipeline and viewport are to be bound again).
uint32_t PendingMipsLocked();
bool MakePendingMipsLocked(u32 texture_va, uint32_t target_width);

// A single-level color target of a resolve destination's format and host
// size, which a multisampled resolve into one with host levels goes through
// (plume's Vulkan resolve writes every level of its destination, from levels
// the source does not have). Its texture and layout only; kept for reuse.
HostTexture *ResolveStagingLocked(const HostTexture *dest);

// Bindless sampler slot matching the device fetch constant's sampler state.
// 'lod_scale' (TextureBinding::lod_scale): a texture with host levels, which
// are sampled to LOD log2(lod_scale) with linear blending between them.
uint32_t SamplerSlotLocked(const TextureFetch &device_fetch, RenderScale lod_scale = {});

// For the memory report: host textures, and of them the resolve destinations
// (which the render resolution scales), with the video memory their levels
// take (estimated from the host format, without the driver's padding).
struct TextureMemory {
  size_t textures = 0;
  uint64_t bytes = 0;
  size_t resolve_targets = 0;
  uint64_t resolve_bytes = 0;
};
TextureMemory TextureMemoryLocked();

// tw2de (2026-10-27): whether a texture with pending memexport results can be made from them on the GPU
// (textures.cpp PlanExportUntile; draw.h ExportUntile): then a draw sampling it does not wait for them.
bool ExportTextureOnGpuLocked(const TextureFetch &fetch);

} // namespace renut::gpu

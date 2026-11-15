/**
 * @file    gpu/render/textures.cpp
 */
#include "renut_engine/gpu/render/textures.h"

#include "renut_engine/frame_timing.h"  // tw2de (2026-11-05, tw2de_diag_vsync_timing: the bookkeeping by part)

#include <algorithm>
#include <string>
#include <fstream>
#include <filesystem>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <xxhash.h>
#include <fmt/format.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/tw2de_renut_tuning.h"

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"

#include <atomic>
#include <chrono>
#include <map>
#include <condition_variable>
#include <functional>
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/backend/video_internal.h"  // tw2de: DumpTextureLocked
#include "cpu/tw2de_texture_sources.h"  // tw2de (2026-10-25): tw2de_diag_texture_sources
#include "cpu/tw2de_glyphs.h"  // tw2de (2026-10-25): the language pack's characters
#include "cpu/tw2de_images.h"  // tw2de (2026-10-25): the language pack's pictures
#include "cpu/tw2de_switch_buttons.h"  // tw2de (2026-10-25): tw2de_switch_buttons

// TW2 (2026-11-12, a test of the green tint): off, a resolve destination whose fetch swizzle does not start with X
// is taken as stored unswapped (the fetch swizzle then exchanges red and blue as written).
REXCVAR_DEFINE_INT32(tw2de_resolve_swap_by_swizzle, 1, "Game/Diagnostics",
                     "A resolve destination whose fetch swizzle's X is not X holds red and blue exchanged (FF XIII's "
                     "rule): 1 every such destination, 0 none, 2 all but the swizzle 60A ones, 3 as 1 and the 16_16_16_16 ones, "
                     "4 as 2 and the 16_16_16_16 ones");
// TW2 (2026-11-12, the green tint): the G-buffer's red is missing for most surfaces (draws sampling the art's DXT1
// textures, while one 64x64 one came out warm); test of the textures' smaller levels: only level 0 made and uploaded.
REXCVAR_DEFINE_BOOL(tw2de_diag_base_level_only, false, "Game/Diagnostics",
                    "TW2 diagnostic (2026-11-12): the game's textures with their first level only (no mipmaps)");
REXCVAR_DEFINE_BOOL(tw2de_dynamic_texture_rows, true, "Game/Enhancements",
                    "TW2 (2026-11-13): a CPU-written (linear, dynamic) texture is uploaded only in the rows whose "
                    "memory pages were written since its last upload (the store cards), not whole every frame. TW2 "
                    "writes two 768x256 RGBA32F textures in turn, 3 MB a frame read, swapped and uploaded. Same texels");
REXCVAR_DEFINE_BOOL(tw2de_diag_tex_made, false, "Game/Diagnostics",
                    "TW2 (2026-11-13): a 10 s summary of the host textures made ([texmade]): how many, by reason, and "
                    "the data (base page) made most often with the header addresses it came under");
REXCVAR_DEFINE_BOOL(tw2de_diag_log, false, "Game/Renut",
                    "tw2de diagnostic: log every new texture ([texnew]) and texture kind, every resolve destination "
                    "([resolvedest]) and memexport stream ([export]), the front buffers and the shadow textures put "
                    "together from resolves");
REXCVAR_DEFINE_BOOL(tw2de_diag_no_export_adopt, false, "Game/Diagnostics",
                    "Diagnostic: a texture made from memexport windows on the GPU is uploaded again from guest memory "
                    "once the results are there, as before 2026-11-05 (textures.cpp, HostTexture::export_adopt)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_diag_cpu_untile, false, "Game/Diagnostics",
                    "Diagnostic: tiled textures from guest memory untiled by the CPU, as before 2026-11-05, not by the "
                    "GPU (textures.cpp Upload, draw.cpp UntileFromGuestLocked)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_diag_no_texture_park, false, "Game/Diagnostics",
                    "Diagnostic: a host texture replaced at its address is released at once, as before 2026-11-05, not "
                    "kept for its content's return (textures.cpp, ParkLocked)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_texture_full_hash, false, "Game/Renut",
                    "tw2de diagnostic: every bound texture's guest bytes hashed in full each frame (besides the "
                    "sampled hash); a change the sampled hash missed is logged ([stale]) and uploaded. Slow");

REXCVAR_DECLARE(double, renut_gpu_profile);
REXCVAR_DEFINE_BOOL(tw2de_lazy_resolve_mips, true, "Game/Enhancements",
                    "tw2de (2026-11-08): the host levels of an upscaled resolve destination (the 1920x1080 "
                    "post-processing textures) made when a draw samples it into a smaller target, not at every resolve "
                    "(most are only sampled 1:1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // tw2de (2026-11-08): backend/gpu_profile.cpp ([mips] report)

namespace renut::gpu {

namespace {

// Fetch constant bits that describe the texture data (not sampling).
constexpr u32 kDescMask[6] = {
    0xFFC003FFu,  // type, signs, pitch, tiled (not clamp modes)
    0xFFFFFFFFu,  // format, endian, stacked, base address
    0xFFFFFFFFu,  // size
    0x0007FFFFu,  // number format, swizzle, exponent adjust (not filters)
    0x000003FCu,  // mip min / max level
    0xFFFFFE00u,  // dimension, packed mips, mip address (not border color)
};

constexpr uint32_t kMaxTextureSize = 8192;

std::unordered_map<u32, std::unique_ptr<HostTexture>> g_textures;
uint32_t g_mips_pending = 0;  // tw2de (2026-11-08): textures with HostTexture::mips_pending_set

// Recent g_textures entries by VA: BindTextureLocked looks up every texture
// of every draw. The map's nodes stay put until erased (ReleaseHostTextureLocked
// drops the entry here first).
struct TextureMemo {
  u32 va = 0;
  std::unique_ptr<HostTexture> *slot = nullptr;
};
constexpr u32 kTextureMemoBits = 10;
TextureMemo g_texture_memo[1u << kTextureMemoBits];

TextureMemo &TextureMemoFor(u32 texture_va) {
  return g_texture_memo[(texture_va * 0x9E3779B1u) >> (32 - kTextureMemoBits)];
}

// g_textures[texture_va], through the memo.
std::unique_ptr<HostTexture> &TextureSlot(u32 texture_va) {
  TextureMemo &memo = TextureMemoFor(texture_va);
  if (memo.slot && memo.va == texture_va)
    return *memo.slot;
  auto &slot = g_textures[texture_va];
  memo = {texture_va, &slot};
  return slot;
}
// Depth resolve destinations with guest words, by base address (4 KB pages).
std::unordered_map<u32, HostTexture *> g_depth_words;
// The last resolve destination into each base address (4 KB pages): D3D
// textures are views of memory, and a game may resolve through one header and
// sample the same memory through another.
std::unordered_map<u32, HostTexture *> g_resolve_targets;
std::unordered_map<uint64_t, uint32_t> g_samplers;  // sampler key -> slot
std::vector<std::unique_ptr<plume::RenderSampler>> g_sampler_objects;
uint64_t g_frame = 1;

// Resources D3D::UnlockResource was called on since the last bind that
// looked (NoteTextureUnlocked; any thread, hence the lock of its own).
std::mutex g_unlocked_mutex;
std::vector<u32> g_unlocked;

// The bindless table filling up (past kEvictPressureSlots of its slots), or
// video memory nearing the budget the OS gives the process (past
// kEvictMemoryShare of it): host textures not bound or resolved into for
// kEvictIdleFrames go, checked every kEvictCheckFrames, and are made again
// from guest memory if bound later. Resolve destinations hold what only the
// GPU has and wait kEvictIdleGpuFrames. The section pools' textures normally
// go with their memory (ReleaseHostTexturesInMemoryLocked); this catches
// whatever else piles up: at 4K, resolve destinations of levels played
// earlier (2.2 GB of them after a few levels, 2026-10-15), while past the
// budget the OS moves allocations out to system memory and the frame rate
// sinks until it frees them.
constexpr uint64_t kEvictCheckFrames = 120;
constexpr uint64_t kEvictIdleFrames = 600;
constexpr uint64_t kEvictIdleGpuFrames = 3600;
constexpr uint32_t kEvictPressureSlots = kTextureDescriptorCount * 3 / 4;
constexpr double kEvictMemoryShare = 0.85;

// Linear textures (see BindTextureLocked): how often a lock-driven texture
// is checked for writes that went around an unlock, and a dynamic one for
// having stopped changing; how long after an unlock the content is watched;
// and how many checks in a row must see a change to make a texture dynamic.
constexpr uint64_t kLinearVerifyFrames = 30;
constexpr uint64_t kUnlockWatchFrames = 8;
constexpr uint32_t kDynamicChanges = 2;

uint32_t NullSlot(TextureDimension dimension) {
  switch (dimension) {
  case TextureDimension::Tex3D:
    return kNullTexture3DSlot;
  case TextureDimension::Cube:
    return kNullTextureCubeSlot;
  default:
    return kNullTextureSlot;
  }
}

// Host dimension the shaders sample it as: stacked 2D arrays go through the
// 3D table, like tfetch3D reads them.
TextureDimension HostDimension(const TextureFetch &fetch) {
  if (fetch.Dimension() == TextureDimension::Tex2D && fetch.Stacked())
    return TextureDimension::Tex3D;
  if (fetch.Dimension() == TextureDimension::Tex1D)
    return TextureDimension::Tex2D;
  return fetch.Dimension();
}

// Host layers of a stacked texture: a 2D array view takes two at least.
uint32_t StackedLayers(const HostTexture &t) {
  return std::max(t.depth, 2u);
}

bool AllSigned(const TextureFetch &fetch) {
  for (u32 c = 0; c < 4; ++c) {
    if (fetch.Sign(c) != TextureSign::Signed)
      return false;
  }
  return true;
}

plume::RenderSwizzle ViewSwizzle(uint32_t component) {
  switch (component) {
  case 0:
    return plume::RenderSwizzle::R;
  case 1:
    return plume::RenderSwizzle::G;
  case 2:
    return plume::RenderSwizzle::B;
  case 3:
    return plume::RenderSwizzle::A;
  case 4:
    return plume::RenderSwizzle::ZERO;
  default:
    return plume::RenderSwizzle::ONE;
  }
}

// Host formats with a single component.
bool SingleComponent(plume::RenderFormat format) {
  using F = plume::RenderFormat;
  switch (format) {
  case F::R8_UNORM:
  case F::R8_SNORM:
  case F::R16_UNORM:
  case F::R16_SNORM:
  case F::R16_FLOAT:
  case F::R32_FLOAT:
  case F::R32_UINT:
  case F::BC4_UNORM:
  case F::BC4_SNORM:
    return true;
  default:
    return false;
  }
}

// The view mapping of a fetch constant swizzle: which stored component each
// shader component reads (0-3 XYZW, 4 zero, 5 one), with X and Z exchanged
// for content stored so (swap_red_blue, see Create). A one-component format
// fetches its value in every component: the game's depth restore
// (ps_beb4ddc38d3b2df5) writes the W of a depth resolve fetched with the
// identity swizzle as its depth, where a host view has 1.
plume::RenderComponentMapping SwizzleMapping(u32 swizzle, bool swap_red_blue,
                                             plume::RenderFormat format) {
  const bool single = SingleComponent(format);
  auto component = [&](uint32_t c) {
    uint32_t source = (swizzle >> (c * 3)) & 7;
    if (single && source < 4)
      source = 0;
    else if (swap_red_blue && (source == 0 || source == 2))
      source ^= 2;
    return ViewSwizzle(source);
  };
  return plume::RenderComponentMapping(component(0), component(1), component(2), component(3));
}

// The bindless slot of 't' seen through 'swizzle': its own view's when that is
// the swizzle it was made with, else a view made for it (kept with 't').
uint32_t SlotForSwizzle(HostTexture &t, u32 swizzle) {
  if (swizzle == t.view_swizzle || !t.texture)
    return t.slot;
  for (const HostTexture::SwizzleView &v : t.swizzle_views) {
    if (v.swizzle == swizzle)
      return v.slot;
  }
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.componentMapping = SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format);
  HostTexture::SwizzleView v;
  v.swizzle = swizzle;
  v.view = t.texture->createTextureView(desc);
  if (!v.view)
    return t.slot;
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    return t.slot;
  }
  Video::SetTextureSlotLocked(v.slot, t.texture.get(), v.view.get());
  t.swizzle_views.push_back(std::move(v));
  return t.swizzle_views.back().slot;
}

// Where a texture's guest data is depends only on the fetch constant bits in
// t.desc, so it is worked out once per texture.
void KnowGuestExtent(const TextureFetch &fetch, HostTexture &t) {
  if (!t.sample_known) {
    TextureGuestExtent(fetch, t.sample_pages, t.sample_sizes);
    t.sample_known = true;
  }
}

// The ends SampleHash reads of a region, and the spans between them.
constexpr uint32_t kSampleEndBytes = 256;
constexpr uint32_t kSampleProbes = 8;
constexpr uint32_t kSampleProbeBytes = 64;

// Cheap content fingerprint of a tiled texture: the first and last 256 bytes
// of the base level and of the mip chain, and spans spread through each. The
// game streams textures into memory another one used, with the same header
// (format, size, address): masks that are white at the edges (the hill's
// blend mask after the clearing's ring stencil) have the same ends, so the
// ends alone kept the old texture. Every bound texture is checked each frame
// and its guest memory is mostly out of the cache, so the cost is the lines
// read: eight spans, gathered and hashed at once. (32 spans hashed one at a
// time were a tenth of the render thread.)
uint64_t SampleHash(const TextureFetch &fetch, HostTexture &t) {
  KnowGuestExtent(fetch, t);
  alignas(64) uint8_t gathered[2 * (2 * kSampleEndBytes + kSampleProbes * kSampleProbeBytes)];
  uint32_t at = 0;
  for (u32 i = 0; i < 2; ++i) {
    const uint32_t page = t.sample_pages[i], size = t.sample_sizes[i];
    if (!page || !size)
      continue;
    const auto *data = GuestPhysicalPtr<const uint8_t>(page << 12);
    const uint32_t n = std::min<uint32_t>(size, kSampleEndBytes);
    std::memcpy(gathered + at, data, n);
    std::memcpy(gathered + at + n, data + size - n, n);
    at += 2 * n;
    if (size > 2 * n + kSampleProbeBytes) {
      const uint32_t span = size - 2 * n - kSampleProbeBytes;
      for (uint32_t p = 1; p <= kSampleProbes; ++p) {
        const uint32_t offset = n + uint32_t(uint64_t(span) * p / (kSampleProbes + 1));
        std::memcpy(gathered + at, data + offset, kSampleProbeBytes);
        at += kSampleProbeBytes;
      }
    }
  }
  return at ? tw2de::renut_gpu::ContentHash64(gathered, at) : 0;  // tw2de: tw2de_renut_pmull_hash
}

// Fingerprint of every guest byte of a texture, for the CPU-written linear
// ones, whose writes land anywhere: N&B's font cache (FontCache.cpp,
// sub_82B8F848) inflates each new glyph into its cell of a 1920x1920 texture
// between D3DLineTexture_LockRect and D3DArrayTexture_UnlockRect.
uint64_t ContentHash(const TextureFetch &fetch, HostTexture &t) {
  KnowGuestExtent(fetch, t);
  uint64_t hash = 0;
  for (u32 i = 0; i < 2; ++i) {
    const uint32_t page = t.sample_pages[i], size = t.sample_sizes[i];
    if (page && size)
      hash = tw2de::renut_gpu::ContentHash64(GuestPhysicalPtr<const uint8_t>(page << 12), size, hash);  // tw2de
  }
  return hash;
}

// tw2de (2026-10-19, render-thread proposal 1, tw2de_renut_store_cards): whether no page of the texture's guest
// extent (both ranges; their cards are read and cleared) was written since interval 'proven'.
bool ExtentClean(HostTexture &t, uint64_t proven) {
  bool clean = proven != UINT64_MAX;
  for (u32 i = 0; i < 2; ++i) {
    if (t.sample_pages[i] && t.sample_sizes[i])
      clean &= GuestRangeCardsClean(GuestPhysicalPtr<const u8>(t.sample_pages[i] << 12), t.sample_sizes[i], proven);
  }
  return clean;
}

// The content check's hash (SampleHash of a tiled texture, ContentHash of a linear one), or the last one taken when
// the cards prove the bytes unchanged since: then the bytes are those hashed, and so would the hash be.
uint64_t CheckedHash(const TextureFetch &fetch, HostTexture &t, bool sampled) {
  KnowGuestExtent(fetch, t);
  const bool cards = tw2de::renut_gpu::StoreCardsOn();
  if (cards && ExtentClean(t, t.hash_interval)) {
    t.hash_interval = CardsInterval();
    // The bytes the hash would have read: all of a linear texture; SampleHash's ends and spans of a tiled one.
    uint64_t bytes = 0;
    for (u32 i = 0; i < 2; ++i) {
      const uint32_t size = t.sample_pages[i] ? t.sample_sizes[i] : 0;
      if (!sampled) {
        bytes += size;
      } else if (size) {
        const uint32_t n = std::min<uint32_t>(size, kSampleEndBytes);
        bytes += 2 * n + (size > 2 * n + kSampleProbeBytes ? kSampleProbes * kSampleProbeBytes : 0);
      }
    }
    if (tw2de::renut_gpu::StoreCardsVerifyOn()) {
      const uint64_t hash = sampled ? SampleHash(fetch, t) : ContentHash(fetch, t);
      if (hash != t.last_hash) {
        static uint32_t logged = 0;
        if (logged++ < 32)
          RGPU_WARN("[cards] texture {:08X} (physical {:08X}, {} bytes) changed under clean cards: a writer of guest "
                    "memory that marks none (tw2de_renut_store_cards_verify)",
                    t.guest_va, t.sample_pages[0] << 12, bytes);
        tw2de::renut_gpu::CountTextureCards(bytes, 3);
        t.last_hash = hash;
        return hash;
      }
    }
    tw2de::renut_gpu::CountTextureCards(bytes, 0);
    return t.last_hash;
  }
  const uint64_t hash = sampled ? SampleHash(fetch, t) : ContentHash(fetch, t);
  t.last_hash = hash;
  t.hash_interval = cards ? CardsInterval() : UINT64_MAX;
  if (cards)
    tw2de::renut_gpu::CountTextureCards(0, 1);
  return hash;
}

struct LevelSource {
  const uint8_t *base = nullptr;  // start of the level's storage (or its packed tail)
  uint32_t pitch_blocks = 0;      // blocks per row of the storage
  uint32_t height_blocks = 0;     // rows of blocks per depth slice (3D)
  uint32_t x_blocks = 0, y_blocks = 0, z_offset = 0;  // offset inside a packed tail
};

// Untiles/copies one subresource (level, slice) into 'dst' rows of host blocks.
// Tiled rows are copied a run of blocks at a time where the tiling keeps
// them together, as the game's UntileSurface does.
void ReadLevel(const TextureFormatInfo &info, const TextureFetch &fetch, const LevelSource &src,
               uint32_t blocks_w, uint32_t blocks_h, uint32_t depth, uint8_t *dst,
               uint32_t dst_row_pitch, uint32_t dst_slice_pitch) {
  const uint32_t gbpb = info.guest_block_bytes;
  const uint32_t hbpb = info.host_block_bytes;
  const uint32_t bpb_log2 = uint32_t(std::countr_zero(gbpb));
  const u32 endian = fetch.Endian();
  const bool is_3d = fetch.Dimension() == TextureDimension::Tex3D;
  const uint32_t run = is_3d ? 1 : TiledRunBlocks(bpb_log2);

  // tw2de (2026-11-06): blocks kept as they are are gathered straight into the staging row and swapped there (no
  // row buffer, no second copy); the others through a per-thread row buffer (no allocation per level).
  const bool direct = info.conversion == TexelConversion::Copy && hbpb == gbpb;
  thread_local std::vector<uint8_t> t_row;
  if (!direct && t_row.size() < size_t(blocks_w) * gbpb)
    t_row.resize(size_t(blocks_w) * gbpb);
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < blocks_h; ++y) {
      const uint32_t sy = y + src.y_blocks;
      const uint32_t sz = z + src.z_offset;
      uint8_t *out = dst + size_t(z) * dst_slice_pitch + size_t(y) * dst_row_pitch;
      uint8_t *row = direct ? out : t_row.data();
      const size_t row_size = size_t(blocks_w) * gbpb;
      if (fetch.Tiled()) {
        for (uint32_t x = 0; x < blocks_w;) {
          const uint32_t sx = x + src.x_blocks;
          const uint32_t offset =
              is_3d ? TiledOffset3D(sx, sy, sz, src.pitch_blocks, src.height_blocks, bpb_log2)
                    : TiledOffset2D(sx, sy, src.pitch_blocks, bpb_log2);
          const uint32_t count = std::min(run - sx % run, blocks_w - x);
          std::memcpy(row + size_t(x) * gbpb, src.base + offset, size_t(count) * gbpb);
          x += count;
        }
      } else {
        const size_t offset = (size_t(sz) * src.height_blocks + sy) * src.pitch_blocks * gbpb +
                              size_t(src.x_blocks) * gbpb;
        std::memcpy(row, src.base + offset, row_size);
      }
      SwapEndian(endian, row, row, row_size);
      if (direct)
        continue;
      if (info.conversion == TexelConversion::Copy)
        std::memcpy(out, row, size_t(blocks_w) * hbpb);
      else if (info.decoded_blocks)
        DecodeBlockRow(info.conversion, row, out, blocks_w, dst_row_pitch / info.block_size);
      else
        ConvertTexels(info.conversion, row, out, blocks_w);
    }
  }
}

// tw2de (2026-10-28): a large level untiled by the render thread and two helpers (cores 0 and 2, the render thread's
// priority), a third of its rows each. The 2026-10-28 load_detail run: an area load's uploads were 124-311 ms a frame,
// nearly all of it ReadLevel (a 4096x4096 DXT1 level 38 ms, 2048x2048 ones 12-17 ms), while the game waited.
class UntileHelpers {
public:
  static constexpr u32 kParts = 3;  // the render thread is part 0

  // tw2de (2026-11-05, seen16): fn takes its work from a counter of its own (any part may run any piece, or none):
  // the render thread's fn(0) does whatever the helpers did not take, and only the helpers that joined are waited for.
  void Run(const std::function<void(u32)> &fn) {
    Start();
    {
      std::lock_guard lock(mutex_);
      fn_ = &fn;
      busy_ = 0;
      active_ = true;
      ++generation_;
    }
    wake_.notify_all();
    fn(0);
    std::unique_lock lock(mutex_);
    active_ = false;
    done_.wait(lock, [this] { return busy_ == 0; });
    fn_ = nullptr;
  }

  void Loop(u32 index) {
    tw2de::renut_gpu::OnCopyHelperStart(index);
    uint64_t seen = 0;
    for (;;) {
      const std::function<void(u32)> *fn;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
        if (!active_)
          continue;  // that run is over
        fn = fn_;
        ++busy_;
      }
      (*fn)(index + 1);
      {
        std::lock_guard lock(mutex_);
        --busy_;
      }
      done_.notify_one();
    }
  }

private:
  void Start();
  bool started_ = false;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  uint64_t generation_ = 0;
  u32 busy_ = 0;         // helpers inside the current run's fn
  bool active_ = false;  // a run is taking helpers
  const std::function<void(u32)> *fn_ = nullptr;
};
UntileHelpers g_untile_helpers;

void UntileHelpers::Start() {
  if (started_)
    return;
  started_ = true;
  tw2de::renut_gpu::StartLongLivedThread([] { g_untile_helpers.Loop(0); }, 64 << 10);
  tw2de::renut_gpu::StartLongLivedThread([] { g_untile_helpers.Loop(1); }, 64 << 10);
}

}  // namespace

// tw2de (2026-11-05, seen15): the helpers for other render-thread copies too (draw.cpp FlushVertexFillsLocked).
void RunOnCopyHelpers(const std::function<void(u32)> &fn) { g_untile_helpers.Run(fn); }

void ParallelMemcpy(void *dst, const void *src, size_t bytes) {
  if (bytes < kParallelMemcpyBytes) {
    std::memcpy(dst, src, bytes);
    return;
  }
  // Pieces of a twelfth (at least 64 KB) on 64-byte boundaries, taken in turn (UntileHelpers::Run).
  const size_t piece = std::max<size_t>(64u << 10, (bytes / 12 + 63u) & ~size_t(63));
  std::atomic<size_t> next{0};
  const std::function<void(u32)> part = [&](u32) {
    for (size_t begin; (begin = next.fetch_add(piece, std::memory_order_relaxed)) < bytes;)
      std::memcpy(static_cast<u8 *>(dst) + begin, static_cast<const u8 *>(src) + begin, std::min(piece, bytes - begin));
  };
  g_untile_helpers.Run(part);
}

namespace {

// ReadLevel, split by rows over the helpers when the level is large (256 KB of guest blocks or more, one slice).
// A level ReadLevelParallel splits in bands (else read in one piece).
bool LevelIsLarge(const TextureFormatInfo &info, uint32_t blocks_w, uint32_t blocks_h, uint32_t depth) {
  return depth == 1 && blocks_h >= 3 * 16 && uint64_t(blocks_w) * blocks_h * info.guest_block_bytes >= (256u << 10);
}
// tw2de (2026-11-06): a texture's small levels together under this: read on the render thread alone.
constexpr uint64_t kSmallReadsParallelBytes = 16u << 10;
uint32_t g_small_read_levels = 0;  // levels read as small ones (the [burst] line's count)

void ReadLevelParallel(const TextureFormatInfo &info, const TextureFetch &fetch, const LevelSource &src,
                       uint32_t blocks_w, uint32_t blocks_h, uint32_t depth, uint8_t *dst, uint32_t dst_row_pitch,
                       uint32_t dst_slice_pitch) {
  if (!LevelIsLarge(info, blocks_w, blocks_h, depth)) {
    ReadLevel(info, fetch, src, blocks_w, blocks_h, depth, dst, dst_row_pitch, dst_slice_pitch);
    return;
  }
  // tw2de (2026-11-05, seen16): 12 bands taken in turn (UntileHelpers::Run: a helper may join late or not at all).
  constexpr u32 kBands = 4 * UntileHelpers::kParts;
  std::atomic<u32> next{0};
  const std::function<void(u32)> part = [&](u32) {
    for (u32 k; (k = next.fetch_add(1, std::memory_order_relaxed)) < kBands;) {
      const uint32_t y0 = blocks_h * k / kBands, y1 = blocks_h * (k + 1) / kBands;
      if (y1 <= y0)
        continue;
      LevelSource band = src;
      band.y_blocks += y0;
      ReadLevel(info, fetch, band, blocks_w, y1 - y0, 1, dst + size_t(y0) * dst_row_pitch, dst_row_pitch,
                dst_slice_pitch);
    }
  };
  g_untile_helpers.Run(part);
}

// Staging data read from guest memory at submission rather than at the draw,
// the way the GPU reads a texture when it executes (see BindTextureLocked).
struct PendingFill {
  TextureFormatInfo info;
  TextureFetch fetch;
  LevelSource src;
  uint32_t blocks_w, blocks_h, depth;
  uint8_t *dst;
  uint32_t row_pitch, slice_pitch;
};
std::vector<PendingFill> g_pending_fills;

// tw2de (2026-10-27): the levels of a texture made from memexport windows on the GPU (draw.h ExportUntile):
// The game's texture kernels write DXT textures (formats 18 and 20, tiled 2D, 256-512 KB) into 32 MB memexport windows,
// and every draw sampling one waited for the GPU and then for the CPU to take the windows back to guest memory
// (30-140 ms each, about ten an area load). A level with pending exports is untiled from the newest window holding
// it, one without from guest memory as before. Only whole blocks the host keeps as they are (no conversion), tiled,
// 2D, one slice; any texture else (or a level two windows share) is not made this way, and its draw waits as before.
struct ExportUntilePlan {
  u32 count = 0;
  u32 slices = 1;  // 6 for a cube map (its faces, slice_bytes apart)
  bool gpu[16] = {};
  ExportUntile levels[16];
  u32 slice_bytes[16] = {};
  bool complete = true;  // tw2de (2026-11-05): no level with pending results left to a record-time read
};

bool PlanExportUntile(const TextureFetch &fetch, ExportUntilePlan &plan) {
  plan = {};
  // tw2de (2026-10-27): cube maps too (6 faces, each a 2D slice of the level): two of the game's kernel-made
  // textures are (64x64x6 DXT5, 256x256x6 DXT1).
  const bool cube = fetch.Dimension() == TextureDimension::Cube;
  // tw2de (2026-10-30): and linear 2D levels when the windows are guest memory (copied and swapped on the GPU: the
  // loading screen's 320x176 icons were a wait and a CPU upload each frame).
  const bool zero_copy = ExportsZeroCopyLocked();
  if (!IsDeko3D() || (!fetch.Tiled() && !(zero_copy && !cube)) ||
      (fetch.Dimension() != TextureDimension::Tex2D && !cube) || fetch.Stacked())
    return false;
  const TextureFormatInfo info = GetTextureFormatInfo(fetch.Format(), AllSigned(fetch));
  if (info.conversion != TexelConversion::Copy || info.decoded_blocks || info.host == plume::RenderFormat::UNKNOWN ||
      info.host_block_bytes != info.guest_block_bytes || !std::has_single_bit(info.guest_block_bytes))
    return false;
  const u32 log2b = u32(std::countr_zero(info.guest_block_bytes));
  if (log2b < 2 || log2b > 4)
    return false;
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  if (depth != (cube ? 6u : 1u))
    return false;
  plan.slices = depth;
  const u32 size_levels = 32 - u32(std::countl_zero(std::max(width, height)));
  const u32 levels = REXCVAR_GET(tw2de_diag_base_level_only) ? 1u : std::min({fetch.MaxMipLevel() + 1, size_levels, 16u});
  u32 block_width, block_height;
  FormatBlockSize(fetch.Format(), block_width, block_height);
  const u32 bs = info.block_size;
  u32 gpu_levels = 0;
  for (u32 level = 0; level < levels; ++level) {
    const TextureLevelLayout layout = GetTextureLevelLayout(fetch, level);
    if (!layout.page)
      continue;
    const u32 base = (layout.page << 12) + layout.offset;
    const u32 bytes = layout.slice_bytes * depth;  // every face of a cube map
    if (!ExportsPendingForRangeLocked(base, bytes))
      continue;  // guest memory has it
    ExportUntile &u = plan.levels[level];
    if (!ExportWindowForRangeLocked(base, bytes, u.slot, u.source_offset)) {
      // tw2de (2026-10-30, tw2de_memexport_zero_copy): not a kernel's output the GPU may read in place
      // (draw.cpp ZeroCopyReadOnGpuLocked): uploaded from guest memory as the draw is recorded, as the game's fence
      // promises; no wait (DrawNeedsExportFlush agrees). The first zero-copy build read every level of a pending window
      // on the GPU at execution time, and every tiled level of 64 KB or more from guest memory too, pending or not: the
      // bind path took "made on the GPU" for "pending" and uploaded 60-70 textures a frame, every frame
      // (tw2de_022.log), and the loading screen's icons were read after the game had moved on.
      if (zero_copy) {
        plan.complete = false;
        continue;
      }
      return false;  // pending, but no one window holds it all
    }
    u.linear = !fetch.Tiled();
    plan.slice_bytes[level] = layout.slice_bytes;
    const u32 w = std::max(1u, width >> level), h = std::max(1u, height >> level);
    u.pitch_blocks = layout.pitch_texels / block_width;
    u.x_blocks = layout.tail_x_blocks;
    u.y_blocks = layout.tail_y_blocks;
    u.blocks_w = (w + bs - 1) / bs;
    u.blocks_h = (h + bs - 1) / bs;
    u.log2_block_bytes = log2b;
    u.endian = fetch.Endian();
    u.row_pitch = (u.blocks_w * info.host_block_bytes + 255u) & ~255u;
    plan.gpu[level] = true;
    ++gpu_levels;
  }
  plan.count = levels;
  return gpu_levels != 0;
}

} // namespace

bool ExportTextureOnGpuLocked(const TextureFetch &fetch) {
  ExportUntilePlan plan;
  return PlanExportUntile(fetch, plan);
}

// tw2de (2026-10-28): the render thread's time in Upload (the [burst] line, draw.cpp).
std::atomic<uint64_t> g_texture_upload_ns{0};

uint64_t TakeTextureUploadNs() {
  return g_texture_upload_ns.exchange(0, std::memory_order_relaxed);
}

// tw2de (2026-10-28): the uploads since the last report, for the [burst] line (TakeTextureUploadReport): how many,
// their time, the part of it untiling on the CPU (ReadLevel) and the deferred fills (FlushTextureFillsLocked), and the
// slowest few. Render thread only.
struct UploadTop {
  double ms = 0.0, read_ms = 0.0;
  u32 va = 0, width = 0, height = 0, depth = 0, mips = 0, format = 0;
  bool deferred = false;
};
struct UploadStats {
  uint32_t count = 0, deferred = 0, fills = 0;
  double ms = 0.0, read_ms = 0.0, fill_ms = 0.0;
  UploadTop top[4];
} g_upload_stats;
double g_upload_read_ms = 0.0;  // the current Upload's ReadLevel time

// tw2de (2026-11-05, the texture bursts): why host textures are made and uploaded, per frame, and how many uploads
// carry bytes a host texture already had (the same layout and content hash: a content cache would have saved them).
// A [texburst] line for a frame whose texture work came to 5 ms or more (tw2de_diag_vsync_timing). Render thread.
namespace {
uint64_t HostTextureBytes(const HostTexture &t);  // further down
}  // namespace
enum TexWhy : uint32_t {
  kWhyNew = 0,     // a header address never seen (or seen so long ago it was forgotten)
  kWhyDesc,        // the header's description changed (another texture at that address)
  kWhyScale,       // its resolve scale changed
  kWhyIdle,        // released before as idle (EvictIdleTexturesLocked), bound again
  kWhyCold,        // released before as cold under the memory reserve (EvictColdTexturesLocked)
  kWhyMemory,      // released when its memory was freed or replaced (ReleaseHostTexturesInMemoryLocked)
  kWhyCount
};
constexpr const char *kWhyNames[kWhyCount] = {"new address", "description changed", "scale changed",
                                              "released as idle", "released as cold", "memory replaced"};
struct TexBurst {
  uint32_t made[kWhyCount] = {};
  double make_ms = 0.0;
  uint32_t uploads_new = 0, uploads_changed = 0;  // uploads of made textures; of kept ones whose content changed
  uint32_t dup_other = 0, dup_same = 0;  // uploads whose content a texture had: at another address / at this one
  uint64_t dup_bytes = 0;
  double dup_ms = 0.0;
  uint32_t restored = 0;  // tw2de (2026-11-05): parked host textures taken back in place of an upload
  uint64_t restored_bytes = 0;
  uint32_t adopted = 0;  // tw2de (2026-11-05): made from memexport windows, the results' hash taken (no upload)
  uint64_t adopted_bytes = 0;
  uint32_t gpu_levels = 0;  // tw2de (2026-11-05): levels untiled on the GPU, their guest bytes, the CPU's copy of them
  uint64_t gpu_bytes = 0;
  double gpu_copy_ms = 0.0;
  // tw2de (2026-11-06): the rest of the bind's texture time at an area entry (run 085a600a: textures 25.8 ms on the
  // render thread, uploads 14.5): the content checks before an upload decision, the composite copies.
  uint32_t checks = 0, composites = 0;
  double check_ms = 0.0, composite_ms = 0.0;
  // tw2de (2026-11-06): whole binds (BindTextureLocked, outermost calls), those that made a host texture and the rest.
  uint32_t binds_made = 0, binds_kept = 0;
  double bind_made_ms = 0.0, bind_kept_ms = 0.0;
} g_texburst;
uint32_t g_bind_depth = 0;  // BindTextureLocked's own nesting (it calls itself after taking a parked texture back)
std::unordered_map<u32, uint8_t> g_tex_released;  // header address -> TexWhy it was released for
std::unordered_map<uint64_t, u32> g_tex_contents;  // layout + content hash -> the header address uploaded with it

void NoteTextureReleased(u32 va, TexWhy why) {
  if (g_tex_released.size() >= 65536)
    g_tex_released.clear();
  g_tex_released[va] = uint8_t(why);
}

// The bytes a texture's content check hashed, with the layout they were read as (not the address).
uint64_t ContentKey(const u32 desc[6], uint64_t hash) {
  const u32 words[5] = {desc[0], desc[1] & 0xFFFu, desc[2], desc[5] & 0xFFFu, desc[3]};
  return XXH3_64bits_withSeed(words, sizeof(words), hash);
}

// tw2de (2026-11-05, the texture bursts): parked host textures. Run 8b92fd2c: in play the game re-described 30-35
// textures at their addresses (frames 3949-3950: "description changed") and then brought the content back, and the
// renderer uploaded it again: 35 uploads of 36.7 MB in 69 ms at frame 3951, 22 of 33 MB in 43 ms at 4655, all
// "content a host texture had already, at the same address". A host texture replaced at its address (another
// description there, or its content changed) now waits here a while; when that address asks for an upload whose
// layout and content hash are a parked one's, that one is taken back instead. The same content check as the cache's
// own: a texture kept at its address is not uploaded again while that hash stays the same. Only textures from guest
// memory (none a resolve or the GPU wrote). At most kParkCount / kParkBytes (the oldest go first past them), with no
// time limit; all go when the GPU memory or the bindless table runs low (the evictions).
struct ParkedTexture {
  u32 va = 0;
  uint64_t key = 0, frame = 0, bytes = 0;
  std::unique_ptr<HostTexture> t;
};
constexpr size_t kParkCount = 64;
constexpr uint64_t kParkBytes = 96ull << 20;
std::vector<ParkedTexture> g_parked;
uint64_t g_parked_bytes = 0;

namespace {
void Retire(HostTexture &t);  // further down
}  // namespace

bool ParkOn() {
  static const bool on = !REXCVAR_GET(tw2de_diag_no_texture_park);
  return on;
}

bool Parkable(const HostTexture &t) {
  return ParkOn() && t.texture && t.uploaded && t.content_hash && !t.gpu_written && !t.render_target && !t.words &&
         !t.handed_from && !t.held_current;
}

void UnparkAtLocked(size_t i, bool retire) {
  if (retire && g_parked[i].t)
    Retire(*g_parked[i].t);
  g_parked_bytes -= g_parked[i].bytes;
  g_parked.erase(g_parked.begin() + std::ptrdiff_t(i));
}

void DropParkedLocked() {
  while (!g_parked.empty())
    UnparkAtLocked(g_parked.size() - 1, true);
}

// 't' leaves its address (the slot is empty after). Requires Parkable(*t).
void ParkLocked(u32 va, std::unique_ptr<HostTexture> &slot) {
  ++g_texture_epoch;  // as Retire does: bound sets made with it are made again
  ParkedTexture p;
  p.va = va;
  p.key = ContentKey(slot->desc, slot->content_hash);
  p.frame = g_frame;
  p.bytes = HostTextureBytes(*slot);
  p.t = std::move(slot);
  g_parked_bytes += p.bytes;
  g_parked.push_back(std::move(p));
  while (!g_parked.empty() && (g_parked.size() > kParkCount || g_parked_bytes > kParkBytes))
    UnparkAtLocked(0, true);  // the oldest first
}

// The parked texture of 'va' with this layout and content, put back in 'slot' (whose texture is parked or retired).
bool TakeParkedLocked(u32 va, std::unique_ptr<HostTexture> &slot, const u32 desc[6], uint64_t hash) {
  if (g_parked.empty() || !slot)
    return false;
  const uint64_t key = ContentKey(desc, hash);
  for (size_t i = g_parked.size(); i-- > 0;) {
    ParkedTexture &p = g_parked[i];
    if (p.va != va || p.key != key || !p.t || p.t->scale != slot->scale ||
        std::memcmp(p.t->desc, desc, sizeof(p.t->desc)) != 0)
      continue;
    std::unique_ptr<HostTexture> back = std::move(p.t);
    const uint64_t bytes = p.bytes;
    UnparkAtLocked(i, false);
    HostTexture &now = *slot;
    // The content check's state is the texture's now (the cards' interval of the hash just taken).
    back->last_hash = now.last_hash;
    back->hash_interval = now.hash_interval;
    back->content_hash = hash;
    back->uploaded = true;
    back->checked_frame = g_frame;
    back->used_frame = g_frame;
    back->composite_copied = ~0ull;
    if (Parkable(now)) {
      ParkLocked(va, slot);
    } else {
      Retire(now);
      slot.reset();
    }
    slot = std::move(back);
    ++g_texture_epoch;
    ++g_texburst.restored;
    g_texburst.restored_bytes += bytes;
    return true;
  }
  return false;
}

void TexBurstReportLocked(uint64_t frame) {
  TexBurst b = g_texburst;
  g_texburst = {};
  // The frame's upload time: draw.cpp's [burst] stats take the sum (TakeTextureUploadNs) right after this, each frame.
  const double frame_upload_ms = double(g_texture_upload_ns.load(std::memory_order_relaxed)) / 1e6;
  if (!tw2de::frame_timing::On() ||
      (b.make_ms + frame_upload_ms < 5.0 && b.restored_bytes + b.adopted_bytes < (4u << 20)))
    return;
  static uint32_t logged = 0;
  if (logged++ >= 200)
    return;
  std::string made;
  uint32_t made_all = 0;
  for (uint32_t i = 0; i < kWhyCount; ++i) {
    made_all += b.made[i];
    if (b.made[i])
      made += fmt::format("{}{} {}", made.empty() ? "" : ", ", kWhyNames[i], b.made[i]);
  }
  RGPU_INFO("[texburst] frame {}: host textures made {} in {:.1f} ms ({}); uploads {:.1f} ms: {} of made textures, {} "
            "of kept ones changed; uploads whose content a host texture had already: {} from another address, {} at "
            "the same address ({:.1f} MB, {:.1f} ms); kept host textures taken back instead of an upload: {} ({:.1f} MB); "
            "levels untiled on the GPU {} ({:.1f} MB, copied for it in {:.1f} ms); made from memexport windows and kept "
            "when the results came (no upload) {} ({:.1f} MB); content checks {} in {:.1f} ms, composite copies {} in "
            "{:.1f} ms; binds that made a texture {} in {:.1f} ms (uploads included), the other binds {} in {:.1f} ms",
            frame, made_all, b.make_ms, made.empty() ? "-" : made, frame_upload_ms, b.uploads_new, b.uploads_changed,
            b.dup_other, b.dup_same, double(b.dup_bytes) / (1 << 20), b.dup_ms, b.restored,
            double(b.restored_bytes) / (1 << 20), b.gpu_levels, double(b.gpu_bytes) / (1 << 20), b.gpu_copy_ms,
            b.adopted, double(b.adopted_bytes) / (1 << 20), b.checks, b.check_ms, b.composites, b.composite_ms,
            b.binds_made, b.bind_made_ms, b.binds_kept, b.bind_kept_ms);
}

std::string TakeTextureUploadReport() {
  UploadStats st = g_upload_stats;
  g_upload_stats = {};
  if (!st.count && !st.fills)
    return {};
  const uint32_t small = g_small_read_levels;
  g_small_read_levels = 0;
  std::string out = fmt::format("{} uploads ({} with their reads deferred) {:.1f} ms, of it untiling on the CPU {:.1f} "
                                "ms ({} small levels, a texture's together over the copy helpers); {} deferred reads "
                                "{:.1f} ms; slowest:",
                                st.count, st.deferred, st.ms, st.read_ms, small, st.fills, st.fill_ms);
  for (const UploadTop &t : st.top) {
    if (t.ms <= 0.0)
      continue;
    out += fmt::format(" {:08X} {}x{}x{} format {} mips {}{} {:.1f} ms (untile {:.1f})", t.va, t.width, t.height,
                       t.depth, t.format, t.mips, t.deferred ? " deferred" : "", t.ms, t.read_ms);
  }
  return out;
}

double g_last_upload_ms = 0.0;  // tw2de (2026-11-05): the last Upload's ms ([texburst])

namespace {

bool UploadUntimed(HostTexture &t, const TextureFetch &fetch, bool defer_read);

// tw2de (2026-10-28): UploadUntimed, timed.
// TW2 (2026-11-13, tw2de_diag_tex_made): every upload by texture, and the deferred fills' bytes; a 10 s summary.
namespace {
struct TexUploadEntry {
  uint32_t count = 0, deferred = 0, fmt = 0, w = 0, h = 0, mips = 0, flags = 0;
  uint64_t bytes = 0;
  double ms = 0.0;
};
std::unordered_map<uint32_t, TexUploadEntry> g_tex_upload_diag;
uint64_t g_tex_fill_diag[3];  // fills, blocks rows x width (texel blocks), ns
uint64_t g_row_upload_diag[4];  // partial uploads, their rows, the rows of the textures, whole uploads instead
void TexUploadReport(bool force_entry);
void TexUploadDiag(const HostTexture &t, const TextureFetch &fetch, bool defer_read, double ms) {
  TexUploadEntry &e = g_tex_upload_diag[t.guest_va];
  ++e.count;
  e.deferred += defer_read;
  e.fmt = u32(t.guest_format);
  e.w = t.width;
  e.h = t.height;
  e.mips = t.mip_levels;
  e.flags = (t.dynamic ? 1u : 0u) | (t.lock_driven ? 2u : 0u) | (fetch.Tiled() ? 4u : 0u) | (t.gpu_written ? 8u : 0u);
  e.ms += ms;
  u32 pages[2] = {}, sizes[2] = {};
  TextureGuestExtent(fetch, pages, sizes);
  e.bytes = uint64_t(sizes[0]) + sizes[1];
  TexUploadReport(false);
}
void TexUploadReport(bool) {
  static auto last = std::chrono::steady_clock::now();
  static uint64_t first_frame = g_frame;
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10))
    return;
  std::vector<std::pair<uint32_t, TexUploadEntry>> top(g_tex_upload_diag.begin(), g_tex_upload_diag.end());
  std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) {
    return a.second.bytes * a.second.count > b.second.bytes * b.second.count;
  });
  uint64_t uploads = 0, bytes = 0;
  double all_ms = 0.0;
  for (const auto &u : top) {
    uploads += u.second.count;
    bytes += u.second.bytes * u.second.count;
    all_ms += u.second.ms;
  }
  std::string line;
  for (size_t i = 0; i < top.size() && i < 14; ++i) {
    const TexUploadEntry &u = top[i].second;
    line += fmt::format(" | {:08X} x{} ({} deferred) fmt {} {}x{} mips {} {} KB flags {:X} {:.1f} ms", top[i].first,
                        u.count, u.deferred, u.fmt, u.w, u.h, u.mips, u.bytes >> 10, u.flags, u.ms);
  }
  RGPU_INFO("[texupload] {} frames: {} uploads of {} textures, {:.1f} MB of guest data, {:.1f} ms recording; deferred "
            "fills {} in {:.1f} ms; dynamic textures by rows: {} uploads of {} rows of {} ({} whole instead){}",
            g_frame - first_frame, uploads, top.size(), double(bytes) / (1 << 20), all_ms, g_tex_fill_diag[0],
            double(g_tex_fill_diag[2]) / 1e6, g_row_upload_diag[0], g_row_upload_diag[1], g_row_upload_diag[2],
            g_row_upload_diag[3], line);
  g_tex_upload_diag.clear();
  g_tex_fill_diag[0] = g_tex_fill_diag[1] = g_tex_fill_diag[2] = 0;
  g_row_upload_diag[0] = g_row_upload_diag[1] = g_row_upload_diag[2] = g_row_upload_diag[3] = 0;
  first_frame = g_frame;
  last = now;
}
}  // namespace

bool Upload(HostTexture &t, const TextureFetch &fetch, bool defer_read) {
  const auto start = std::chrono::steady_clock::now();
  g_upload_read_ms = 0.0;
  const bool ok = UploadUntimed(t, fetch, defer_read);
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
  g_texture_upload_ns.fetch_add(uint64_t(ns), std::memory_order_relaxed);
  // tw2de (2026-10-28): TakeTextureUploadReport.
  const double ms = double(ns) / 1e6;
  g_last_upload_ms = ms;  // tw2de (2026-11-05, [texburst])
  if (REXCVAR_GET(tw2de_diag_tex_made))  // TW2 (2026-11-13)
    TexUploadDiag(t, fetch, defer_read, ms);
  UploadStats &st = g_upload_stats;
  ++st.count;
  st.deferred += defer_read;
  st.ms += ms;
  st.read_ms += g_upload_read_ms;
  for (int i = 0; i < 4; ++i) {
    if (ms > st.top[i].ms) {
      for (int j = 3; j > i; --j)
        st.top[j] = st.top[j - 1];
      st.top[i] = {ms, g_upload_read_ms, t.guest_va, t.width, t.height, t.depth, t.mip_levels,
                   u32(t.guest_format), defer_read};
      break;
    }
  }
  return ok;
}

// Records the copies of every level into 't'. The guest data is read now, or
// when the command list is submitted if 'defer_read'.
bool UploadUntimed(HostTexture &t, const TextureFetch &fetch, bool defer_read) {
  // tw2de (2026-10-25): the game's font atlases get a language pack's characters before their first read.
  // tw2de (2026-10-25): and the pack's translated pictures (cpu/tw2de_images.cpp) over the game's textures; a
  // larger picture (the texture made larger in its header) is uploaded from its own blocks, not from guest memory.
  const std::vector<uint8_t> *picture = nullptr;
  if (t.mip_levels == 1 && t.depth == 1 &&
      (tw2de::GlyphsActive() || tw2de::ImagesActive() || tw2de::SwitchButtonsActive())) {
    const TextureLevelLayout base = GetTextureLevelLayout(fetch, 0);
    if (base.page && !base.in_tail) {
      auto *data = const_cast<uint8_t *>(GuestPhysicalPtr<const uint8_t>((base.page << 12) + base.offset));
      if (fetch.Format() == GpuTextureFormat::FmtDXT1 && fetch.Tiled() && base.pitch_texels == t.width)
        tw2de::GlyphsPatchAtlas(data, t.width, t.height);
      tw2de::SwitchButtonsPatch(data, u32(fetch.Format()), t.width, t.height, fetch.Tiled());  // tw2de: the buttons
      picture = tw2de::ImagesPatch(data, u32(fetch.Format()), t.width, t.height, fetch.Tiled(), base.pitch_texels);
      if (picture && (t.info.conversion != TexelConversion::Copy || t.info.decoded_blocks))
        picture = nullptr;  // only a format the host has (DXT1 / DXT5 blocks as they are)
      // tw2de (2026-10-30, tw2de_memexport_zero_copy): the patches wrote guest memory from this thread and marked
      // no store card: cleaned to memory before the GPU untiles the level from it (PlanExportUntile).
      GuestRangeWrittenByHostLocked((base.page << 12) + base.offset, base.slice_bytes * std::max(1u, t.depth));
    }
  }
  // tw2de (2026-10-25, tw2de_diag_texture_sources): which game file the base level's data came from, logged once.
  if (tw2de::TextureSourcesOn()) {
    const TextureLevelLayout base = GetTextureLevelLayout(fetch, 0);
    if (base.page)
      tw2de::TextureSourcesNoteUpload(t.guest_va, (base.page << 12) + base.offset,
                                       base.slice_bytes * std::max(1u, t.depth), t.width, t.height, t.depth,
                                       u32(fetch.Format()), t.mip_levels, fetch.Tiled(), g_frame);
  }
  // tw2de (2026-10-27): the levels with pending memexport results, from their windows on the GPU.
  ExportUntilePlan export_plan;
  const bool from_exports = !picture && PlanExportUntile(fetch, export_plan);
  // tw2de (2026-11-05): every pending level from its window (HostTexture::export_adopt), unless one falls back.
  bool all_from_windows = from_exports && export_plan.complete;
  t.export_adopt = false;
  // tw2de (2026-10-19, render-thread proposal 1): the interval the guest bytes are read in: now, or at the next
  // kick's flush (FlushTextureFillsLocked, right after FlushVertexFillsLocked starts a new interval; a later one is
  // only more cautious).
  t.read_interval = CardsInterval() + (defer_read ? 1 : 0);
  // tw2de (2026-10-19, render-thread proposal 3): no g_texture_epoch bump: an upload changes content, not a binding.
  t.resolve_signature = 0;  // tw2de (tw2de_renut_resolve_skip): not what a resolve left any more
  u32 block_width, block_height;
  FormatBlockSize(fetch.Format(), block_width, block_height);

  auto &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  static const uint32_t kProfileLabel = gpu_profile::Label("texture uploads");
  gpu_profile::Mark(s, kProfileLabel);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  t.layout = plume::RenderTextureLayout::COPY_DEST;

  const bool is_3d_data = fetch.Dimension() == TextureDimension::Tex3D;
  // tw2de (2026-11-05, GPU untile): a tiled 2D or cube texture whose blocks are copied as they are (the formats the
  // memexport untile kernel takes: PlanExportUntile's) has its larger levels untiled on the GPU: the CPU copies their
  // tiled storage (a straight copy, about a quarter of the untile's time) and stream_convert_cs untiles and swaps it.
  // Run 8b92fd2c: CPU untiling was 35-48 ms of each burst's 36-69 ms of uploads. Read at record time, as before.
  static const bool cpu_untile = REXCVAR_GET(tw2de_diag_cpu_untile);
  const TextureFormatInfo &tinfo = t.info;
  const bool gpu_untile = !cpu_untile && IsDeko3D() && !picture && !defer_read && fetch.Tiled() && !is_3d_data &&
                          !fetch.Stacked() &&
                          (fetch.Dimension() == TextureDimension::Tex2D || fetch.Dimension() == TextureDimension::Cube) &&
                          tinfo.conversion == TexelConversion::Copy && !tinfo.decoded_blocks &&
                          tinfo.host != plume::RenderFormat::UNKNOWN &&
                          tinfo.host_block_bytes == tinfo.guest_block_bytes &&
                          std::has_single_bit(tinfo.guest_block_bytes) && tinfo.guest_block_bytes >= 4 &&
                          tinfo.guest_block_bytes <= 16;
  constexpr uint64_t kGpuUntileMinBytes = 64u << 10;  // smaller levels: the CPU's untile costs less than the dispatch
  // Host array layers (cube faces, stacked layers); a stacked texture's extra
  // layer (StackedLayers) repeats its last one.
  const uint32_t slices = is_3d_data ? 1 : t.stacked ? StackedLayers(t) : t.depth;
  const uint32_t bs = t.info.block_size;
  // tw2de (2026-11-06): the levels too small for ReadLevelParallel's bands (mips, the packed tail, small textures),
  // read after the loop by the render thread and the copy helpers, a level each in turn (the staging is only read by
  // the GPU when the list runs). Run af454352: an area entry's 97 uploads untiled 14.3 ms on the CPU, the large levels on
  // the GPU already.
  struct SmallRead {
    LevelSource src;
    uint32_t blocks_w, blocks_h, d, row_pitch, slice_pitch;
    uint8_t *dst;
    uint64_t bytes;
  };
  std::vector<SmallRead> small_reads;
  for (uint32_t level = 0; level < t.mip_levels; ++level) {
    const uint32_t w = std::max(1u, t.width >> level);
    const uint32_t h = std::max(1u, t.height >> level);
    const uint32_t d = is_3d_data ? std::max(1u, t.depth >> level) : 1;
    const uint32_t blocks_w = (w + bs - 1) / bs;
    const uint32_t blocks_h = (h + bs - 1) / bs;

    // Where the level lives: base or mip memory, possibly inside the packed tail.
    const TextureLevelLayout layout = GetTextureLevelLayout(fetch, level);
    const uint32_t page = layout.page;
    if (!page)
      continue;
    LevelSource src;
    src.pitch_blocks = layout.pitch_texels / block_width;
    src.height_blocks = layout.height / block_height;
    src.x_blocks = layout.tail_x_blocks;
    src.y_blocks = layout.tail_y_blocks;
    src.z_offset = layout.tail_z;
    const uint32_t level_offset = layout.offset;

    const uint32_t row_bytes = blocks_w * t.info.host_block_bytes;
    // Footprint rows are 256-byte aligned: rows of blocks, or for decoded
    // blocks each of the block_size texel rows a row of blocks holds.
    const uint32_t row_align = t.info.decoded_blocks ? 256u * bs : 256u;
    const uint32_t row_pitch = (row_bytes + row_align - 1) / row_align * row_align;
    const uint32_t slice_pitch = row_pitch * blocks_h;
    for (uint32_t slice = 0; slice < slices; ++slice) {
      const uint32_t guest_slice = std::min(slice, t.depth - 1);
      src.base = GuestPhysicalPtr<const uint8_t>((page << 12) + level_offset +
                                                 guest_slice * layout.slice_bytes);
      // tw2de (2026-10-27): a level with pending memexport results, untiled from its window on the GPU.
      const bool pending_level = from_exports && level < export_plan.count && export_plan.gpu[level];
      if (pending_level && !(slices == export_plan.slices && d == 1))
        all_from_windows = false;  // tw2de (2026-11-05): a pending level read from guest memory below
      if (pending_level && slices == export_plan.slices && d == 1) {
        ExportUntile u = export_plan.levels[level];
        u.row_pitch = row_pitch;
        u.source_offset += uint64_t(guest_slice) * export_plan.slice_bytes[level];  // a cube map's face
        plume::RenderBufferReference rows;
        if (!UntileFromExportLocked(u, rows))
          all_from_windows = false;  // read from guest memory below: what it has now
        else {
          cmd = s.command_list;  // the same list (a dispatch does not submit)
          const uint32_t copy_w = std::min(blocks_w * bs, std::max(1u, t.alloc_width >> level));
          const uint32_t copy_h = std::min(blocks_h * bs, std::max(1u, t.alloc_height >> level));
          const uint32_t row_texels = row_pitch / t.info.host_block_bytes * bs;
          cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
                                 plume::RenderTextureCopyLocation::PlacedFootprint(rows.ref, t.info.host, copy_w,
                                                                                   copy_h, 1, row_texels, rows.offset),
                                 0, 0, 0, nullptr);
          continue;
        }
      }
      // tw2de (2026-11-05, GPU untile).
      if (gpu_untile && d == 1 && layout.slice_bytes &&
          uint64_t(blocks_w) * blocks_h * tinfo.guest_block_bytes >= kGpuUntileMinBytes) {
        ExportUntile u;
        u.pitch_blocks = src.pitch_blocks;
        u.x_blocks = src.x_blocks;
        u.y_blocks = src.y_blocks;
        u.blocks_w = blocks_w;
        u.blocks_h = blocks_h;
        u.log2_block_bytes = u32(std::countr_zero(tinfo.guest_block_bytes));
        u.endian = fetch.Endian();
        u.row_pitch = row_pitch;
        plume::RenderBufferReference rows;
        const auto copy_start = std::chrono::steady_clock::now();
        if (UntileFromGuestLocked(u, src.base, layout.slice_bytes, rows)) {
          ++g_texburst.gpu_levels;
          g_texburst.gpu_bytes += layout.slice_bytes;
          g_texburst.gpu_copy_ms +=
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copy_start).count();
          cmd = s.command_list;  // the same list (a dispatch does not submit)
          const uint32_t copy_w = std::min(blocks_w * bs, std::max(1u, t.alloc_width >> level));
          const uint32_t copy_h = std::min(blocks_h * bs, std::max(1u, t.alloc_height >> level));
          const uint32_t row_texels = row_pitch / t.info.host_block_bytes * bs;
          cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
                                 plume::RenderTextureCopyLocation::PlacedFootprint(rows.ref, t.info.host, copy_w,
                                                                                   copy_h, 1, row_texels, rows.offset),
                                 0, 0, 0, nullptr);
          continue;
        }
      }
      UploadAllocation staging = s.uploads[s.frame].Allocate(uint64_t(slice_pitch) * d, 512);
      if (!staging)
        return false;
      auto *dst = static_cast<uint8_t *>(staging.cpu);
      if (picture && level == 0 && slice == 0 && picture->size() >= size_t(blocks_w) * blocks_h * t.info.host_block_bytes) {
        for (uint32_t y = 0; y < blocks_h; ++y)  // tw2de (2026-10-25): a language pack's larger picture
          std::memcpy(dst + size_t(y) * row_pitch, picture->data() + size_t(y) * blocks_w * t.info.host_block_bytes,
                      size_t(blocks_w) * t.info.host_block_bytes);
      } else if (defer_read)
        g_pending_fills.push_back({t.info, fetch, src, blocks_w, blocks_h, d, dst, row_pitch, slice_pitch});
      else if (!LevelIsLarge(t.info, blocks_w, blocks_h, d)) {
        small_reads.push_back({src, blocks_w, blocks_h, d, row_pitch, slice_pitch, dst,
                               uint64_t(blocks_w) * blocks_h * d * t.info.guest_block_bytes});
      } else {
        const auto read_start = std::chrono::steady_clock::now();  // tw2de (2026-10-28)
        ReadLevelParallel(t.info, fetch, src, blocks_w, blocks_h, d, dst, row_pitch, slice_pitch);
        g_upload_read_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read_start).count();
      }
      // Footprints are in texels, block-aligned for compressed formats. Decoded
      // blocks are plain texels, so a level smaller than a block (a small mip)
      // takes only its own part of the decoded block.
      const uint32_t copy_w = std::min(blocks_w * bs, std::max(1u, t.alloc_width >> level));
      const uint32_t copy_h = std::min(blocks_h * bs, std::max(1u, t.alloc_height >> level));
      const plume::RenderBox level_box(0, 0, int32_t(copy_w), int32_t(copy_h), 0, int32_t(d));
      // plume's Vulkan copy takes its extent from the footprint and ignores the
      // box, and Vulkan allows no extent past the level (a compressed one ends
      // at the level's edge instead of a block's): the footprint is the level's
      // part there. D3D12 wants block-aligned footprints, cut by the box.
      const bool vulkan = UsesVulkanBindings();  // tw2de: deko3d copies as Vulkan does (the level's extent)
      const uint32_t row_texels = row_pitch / t.info.host_block_bytes * bs;
      if (vulkan && d > 1 && copy_h != blocks_h * bs) {
        // The footprint's height is also its rows per depth slice there, which
        // the staging has blocks_h * bs of: one depth slice at a time.
        for (uint32_t z = 0; z < d; ++z) {
          cmd->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
              plume::RenderTextureCopyLocation::PlacedFootprint(
                  staging.ref.ref, t.info.host, copy_w, copy_h, 1, row_texels,
                  staging.ref.offset + uint64_t(z) * slice_pitch),
              0, 0, z, nullptr);
        }
      } else {
        cmd->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
            plume::RenderTextureCopyLocation::PlacedFootprint(
                staging.ref.ref, t.info.host, vulkan ? copy_w : blocks_w * bs,
                vulkan ? copy_h : blocks_h * bs, d, row_texels, staging.ref.offset),
            0, 0, 0, !vulkan && t.info.decoded_blocks ? &level_box : nullptr);
      }
    }
  }
  if (!small_reads.empty()) {
    const auto read_start = std::chrono::steady_clock::now();
    uint64_t small_bytes = 0;
    for (const SmallRead &r : small_reads)
      small_bytes += r.bytes;
    auto read_one = [&](const SmallRead &r) {
      ReadLevel(t.info, fetch, r.src, r.blocks_w, r.blocks_h, r.d, r.dst, r.row_pitch, r.slice_pitch);
    };
    if (small_reads.size() < 2 || small_bytes < kSmallReadsParallelBytes) {
      for (const SmallRead &r : small_reads)
        read_one(r);
    } else {
      // The largest first, so the last pieces taken are the short ones.
      std::sort(small_reads.begin(), small_reads.end(),
                [](const SmallRead &a, const SmallRead &b) { return a.bytes > b.bytes; });
      std::atomic<size_t> next{0};
      const std::function<void(u32)> part = [&](u32) {
        for (size_t k; (k = next.fetch_add(1, std::memory_order_relaxed)) < small_reads.size();)
          read_one(small_reads[k]);
      };
      RunOnCopyHelpers(part);
    }
    g_upload_read_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read_start).count();
    g_small_read_levels += uint32_t(small_reads.size());
  }
  t.export_adopt = all_from_windows;  // tw2de (2026-11-05)
  return true;
}

// The host levels a scaled resolve destination gets (HostTexture::extra_levels):
// down to a texel of at least the guest's.
uint32_t ExtraLevels(RenderScale scale) {
  const float factor = scale.Factor();
  return factor > 4.0f ? 3 : factor > 2.0f ? 2 : factor > 1.0f ? 1 : 0;
}

// swap_red_blue: the content was stored with X and Z exchanged (a guest
// resolve does that when the destination's X swizzle is not X). 'scale': see
// HostTexture::scale (content the GPU writes only). host_levels: a color
// resolve destination, which gets HostTexture::extra_levels when it is a
// scaled 2D one.
bool Create(HostTexture &t, const TextureFetch &fetch, bool render_target,
            plume::RenderFormat rt_format, bool swap_red_blue = false, RenderScale scale = {},
            bool host_levels = false) {
  ++g_texture_epoch;  // tw2de (CPU proposal 2)
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  t.dimension = HostDimension(fetch);
  t.guest_format = fetch.Format();
  t.info = GetTextureFormatInfo(fetch.Format(), AllSigned(fetch));
  t.width = width;
  t.height = height;
  t.depth = depth;
  t.scale = scale;
  // No more levels than the size has (GetMaxMipLevels, 0x826554B8).
  const u32 largest = std::max({width, height, fetch.Dimension() == TextureDimension::Tex3D ? depth : 1u});
  const u32 size_levels = 32 - u32(std::countl_zero(largest));
  t.mip_levels = render_target || REXCVAR_GET(tw2de_diag_base_level_only) ? 1 : std::min(fetch.MaxMipLevel() + 1, size_levels);
  t.render_target = render_target;
  plume::RenderFormat format = render_target ? rt_format : t.info.host;
  if (format == plume::RenderFormat::UNKNOWN)
    return false;
  // tw2de (2026-10-25, tw2de_deko3d_native_formats): the first texture of each packed format, once (whether the
  // game uses them, and where to look if one shows wrong colours).
  {
    using F = plume::RenderFormat;
    static std::atomic<uint32_t> seen{0};
    const uint32_t bit = format == F::R10G10B10A2_UNORM ? 1u : format == F::R5G6B5_UNORM ? 2u
                         : format == F::R5G5B5A1_UNORM ? 4u : format == F::R4G4B4A4_UNORM ? 8u : 0u;
    if (bit && !(seen.fetch_or(bit, std::memory_order_relaxed) & bit)) {
      static const char *const kNames[] = {"", "10:10:10:2", "5:6:5", "", "1:5:5:5", "", "", "", "4:4:4:4"};
      RGPU_INFO("[textures] first {} texture in the GPU's own format: {}x{}, guest format {}{} (tw2de_deko3d_native_formats)",
                kNames[bit], width, height, u32(fetch.Format()), render_target ? ", a resolve destination" : "");
    }
  }

  plume::RenderDevice *device = Video::Device();
  plume::RenderTextureDesc desc;
  plume::RenderTextureViewDesc view_desc;
  const uint32_t bs = render_target ? 1 : t.info.block_size;
  const uint32_t w = scale.Size((t.width + bs - 1) / bs * bs);
  const uint32_t h = scale.Size((t.height + bs - 1) / bs * bs);
  t.extra_levels = 0;
  if (render_target && host_levels && t.dimension == TextureDimension::Tex2D &&
      State().texture_mip_ps) {
    // No more than the host size has.
    const uint32_t host_levels_max = 32 - uint32_t(std::countl_zero(std::max(w, h)));
    t.extra_levels = std::min(ExtraLevels(scale), host_levels_max - 1);
    t.mip_levels += t.extra_levels;
  }
  t.stacked = fetch.Dimension() == TextureDimension::Tex2D && fetch.Stacked();
  switch (t.dimension) {
  case TextureDimension::Tex3D:
    if (t.stacked) {
      // Viewed as a 2D array, which takes two layers at least (StackedLayers).
      desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, w, h, 1,
                                               t.mip_levels, StackedLayers(t), format);
      view_desc = plume::RenderTextureViewDesc::Texture2D(format);
    } else {
      desc = plume::RenderTextureDesc::Texture3D(w, h, t.depth, t.mip_levels, format);
      view_desc = plume::RenderTextureViewDesc::Texture3D(format);
    }
    break;
  case TextureDimension::Cube:
    desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, w, h, 1,
                                             t.mip_levels, 6, format,
                                             plume::RenderTextureFlag::CUBE);
    view_desc = plume::RenderTextureViewDesc::TextureCube(format);
    t.depth = 6;
    break;
  default:
    desc = plume::RenderTextureDesc::Texture2D(w, h, t.mip_levels, format);
    view_desc = plume::RenderTextureViewDesc::Texture2D(format);
    t.depth = 1;
    break;
  }
  if (render_target)
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  // tw2de (2026-10-20): whether the game uses stacked (2D array) or volume textures at all, once each (SASRT's
  // deko3d findings: plume's 2D view of a 2D array, and 3D images; docs/deko3d-port-faq.md).
  if (t.dimension == TextureDimension::Tex3D) {
    static std::atomic<bool> logged_stacked{false}, logged_volume{false};
    std::atomic<bool> &logged = t.stacked ? logged_stacked : logged_volume;
    if (!logged.exchange(true, std::memory_order_relaxed))
      RGPU_INFO("[textures] first {} texture: {}x{}x{}, {} levels, host format {}",
                t.stacked ? "stacked (2D array)" : "volume (3D)", w, h, t.depth, t.mip_levels, int(format));
  }
  t.host_format = format;
  t.alloc_width = w;
  t.alloc_height = h;
  // Beyond what the backends create (plume returns an object without a
  // resource then); Xenos textures are at most 8192 wide/high, and the
  // backends' limit is 16384.
  const uint32_t max_size = scale.Factor() > 1.0f ? kMaxTextureSize * 2 : kMaxTextureSize;
  if (w > max_size || h > max_size || t.depth > kMaxTextureSize)
    return false;
  t.texture = device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    ReportDeviceLost(device);
    return false;
  }
  t.texture->setName("renut texture");

  view_desc.mipLevels = t.mip_levels;
  view_desc.componentMapping = SwizzleMapping(fetch.Swizzle(), swap_red_blue, format);
  t.view_desc = view_desc;
  t.view_swizzle = fetch.Swizzle();
  t.view_swap_red_blue = swap_red_blue;
  t.view = t.texture->createTextureView(view_desc);
  if (!t.view)
    return false;
  t.layout = plume::RenderTextureLayout::UNKNOWN;
  t.slot = Video::AllocateTextureSlotLocked();
  if (t.slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
  return true;
}

void MaskedDesc(const TextureFetch &fetch, u32 out[6]) {
  for (int i = 0; i < 6; ++i)
    out[i] = fetch.dword[i] & kDescMask[i];
}

void Retire(HostTexture &t) {
  ++g_texture_epoch;  // tw2de (CPU proposal 2)
  DepthHandOverForgetLocked(t);  // tw2de (2026-10-19, tw2de_deko3d_depth_handover)
  if (t.slot != UINT32_MAX && t.slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.slot);
  t.slot = UINT32_MAX;
  for (HostTexture::SwizzleView &v : t.swizzle_views) {
    Video::FreeTextureSlotLocked(v.slot);
    Video::RetireLocked(std::move(v.view));
  }
  t.swizzle_views.clear();
  if (t.level0_slot != UINT32_MAX && t.level0_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.level0_slot);
  t.level0_slot = UINT32_MAX;
  Video::RetireLocked(std::move(t.level0_view));
  Video::RetireLocked(std::move(t.copy_framebuffer));  // tw2de (2026-10-18)
  for (HostTexture::SliceTarget &st : t.slice_targets) {  // tw2de (2026-11-12)
    Video::RetireLocked(std::move(st.framebuffer));
    Video::RetireLocked(std::move(st.view));
  }
  t.slice_targets.clear();
  if (t.mips_pending_set) {  // tw2de (2026-11-08, tw2de_lazy_resolve_mips)
    t.mips_pending_set = false;
    --g_mips_pending;
  }
  for (HostTexture::MipTarget &m : t.mip_targets) {  // tw2de (2026-11-08)
    if (m.slot != UINT32_MAX && m.slot != kInvalidDescriptor)
      Video::FreeTextureSlotLocked(m.slot);
    Video::RetireLocked(std::move(m.framebuffer));
    Video::RetireLocked(std::move(m.view));
  }
  t.mip_targets.clear();
  Video::RetireLocked(std::move(t.view));
  Video::RetireLocked(std::move(t.texture));
  if (t.words) {
    for (HostTexture::WordsView &v : t.words_views) {  // tw2de (tw2de_gpu_depth_words_view)
      if (v.slot != UINT32_MAX && v.slot != kInvalidDescriptor)
        Video::FreeTextureSlotLocked(v.slot);
      Video::RetireLocked(std::move(v.view));
    }
    t.words_views.clear();
    Video::RetireLocked(std::move(t.depth_framebuffer));
    Retire(*t.words);
    t.words.reset();
    std::erase_if(g_depth_words, [&t](const auto &entry) { return entry.second == &t; });
  }
  if (t.render_target)
    std::erase_if(g_resolve_targets, [&t](const auto &entry) { return entry.second == &t; });
}

// Two descriptions of the same data: the same memory, format, endianness, size
// and dimension (fetch dwords 1, 2 and the dimension in 5). Fetch constants of
// the same memory may still differ in pitch, swizzle or mip fields.
bool SameData(const u32 a[6], const u32 b[6]) {
  constexpr u32 kDimensionMask = 0x600u;  // fetch dword 5 bits 9-10
  return a[1] == b[1] && a[2] == b[2] && (a[5] & kDimensionMask) == (b[5] & kDimensionMask);
}

// The resolve destination another header wrote this texture's memory through,
// when it holds the same data (SameData). Only the GPU has what the resolve
// wrote.
HostTexture *ResolvedAlias(u32 texture_va, const TextureFetch &fetch, const u32 desc[6]) {
  auto it = g_resolve_targets.find(fetch.BasePage());
  if (it == g_resolve_targets.end())
    return nullptr;
  HostTexture *resolved = it->second;
  if (resolved->guest_va == texture_va || !resolved->texture)
    return nullptr;
  return SameData(resolved->desc, desc) ? resolved : nullptr;
}

// tw2de: the resolve destinations a larger 2D texture lies over, each a band of its rows (tiled memory: the rows
// of a texture of the same pitch at a later address are its rows further down). The game resolves its shadow cascades
// into 1024x1024 destinations one after another and samples them as one 1024x2048 texture (24_8, and its 8_8_8_8
// view of the words); renut made that texture from guest memory, which no resolve writes: the shadow test saw
// garbage, the world went black.
struct CompositePart {
  HostTexture *dest = nullptr;  // the resolve destination
  u32 y = 0;                    // its first row in the texture (guest rows)
  bool words = false;           // its depth words (an 8_8_8_8 texture over a depth resolve)
};

uint32_t ResolveComposite(u32 texture_va, const TextureFetch &fetch, CompositePart *parts, uint32_t max_parts) {
  if (g_resolve_targets.empty() || !fetch.Tiled() || fetch.Stacked() || fetch.Dimension() != TextureDimension::Tex2D)
    return 0;
  const GpuTextureFormat format = fetch.Format();
  const bool words_fetch = format == GpuTextureFormat::Fmt8_8_8_8;
  if (FormatBitsPerPixel(format) != 32)
    return 0;
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  const u32 pitch = fetch.PitchTexels() ? fetch.PitchTexels() : (width + 31) & ~31u;
  const u64 row_bytes = u64(pitch) * 4;
  const u32 base = fetch.BasePage();
  const u64 end = u64(base) * 4096 + row_bytes * height;
  uint32_t count = 0;
  for (const auto &[page, dest] : g_resolve_targets) {
    if (!dest || !dest->texture || dest->guest_va == texture_va || page < base || u64(page) * 4096 >= end)
      continue;
    if (dest->dimension != TextureDimension::Tex2D || dest->stacked || dest->width != width)
      continue;
    const u64 offset = u64(page - base) * 4096;
    if (offset % (row_bytes * 32))
      continue;
    const u32 y = u32(offset / row_bytes);
    if (y + dest->height > height || (y == 0 && dest->height == height))
      continue;  // the same size at the same place: ResolvedAlias / DepthWordsSource
    const bool depth_dest = dest->words != nullptr;
    bool words = false;
    if (depth_dest && words_fetch)
      words = true;
    else if (dest->guest_format != format)
      continue;
    if (count < max_parts)
      parts[count++] = CompositePart{dest, y, words};
  }
  return count;
}

// tw2de (2026-11-05, seen18): the parts cover every row of 't' (one level, no slices): nothing of the guest data
// would be left after CopyCompositeLocked.
bool CompositeCoversAll(const TextureFetch &fetch, const HostTexture &t, const CompositePart *parts, uint32_t count) {
  if (t.extra_levels != 0 || t.depth > 1 || count == 0)
    return false;
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  std::pair<u32, u32> rows[8];
  const uint32_t n = std::min<uint32_t>(count, 8);
  for (uint32_t i = 0; i < n; ++i)
    rows[i] = {parts[i].y, parts[i].y + parts[i].dest->height};
  std::sort(rows, rows + n);
  u32 covered = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (rows[i].first > covered)
      return false;  // a gap
    covered = std::max(covered, rows[i].second);
  }
  return covered >= height;
}

// Copies the parts' content into 't' when one has resolved since (or once a frame).
void CopyCompositeLocked(HostTexture &t, CompositePart *parts, uint32_t count) {
  uint64_t key = g_frame * 0x9E3779B97F4A7C15ull;
  for (uint32_t i = 0; i < count; ++i)
    key += parts[i].dest->resolves * (i + 1) + uintptr_t(parts[i].dest);
  if (t.composite_copied == key)
    return;
  // tw2de (2026-11-08): a depth resolve left in its target's image (the depth hand-over) is drawn into its texture
  // first: the copy read the texture, which never had it. The shadow cascades' 1024x2048 24_8 texture was all 1.0
  // (far): no real-time shadows (characters, monsters) anywhere. (The words path draws it in EnsureDepthWordsLocked.)
  int made[8] = {};
  for (uint32_t i = 0; i < count; ++i)
    if (!parts[i].words)
      made[i & 7] = DepthHandOverMaterialiseLocked(*parts[i].dest);
  // tw2de (2026-11-08, shadows diagnostic): each part as found (tw2de_diag_log, 48 lines).
  static uint32_t parts_logged = 0;
  if (REXCVAR_GET(tw2de_diag_log) && parts_logged < 48)
    for (uint32_t i = 0; i < count && parts_logged < 48; ++i, ++parts_logged) {
      const HostTexture &d = *parts[i].dest;
      RGPU_INFO("[composite] frame {}: texture {:08X} {}x{} (format {} host {} scale {}) part {} of {}: {:08X} {}x{} at "
                "row {} (host {} scale {} words {} resolves {} gpu_written {}), hand-over {}",
                g_frame, t.guest_va, t.width, t.height, u32(t.guest_format), u32(t.host_format), t.scale.code, i, count,
                d.guest_va, d.width, d.height, parts[i].y, u32(d.host_format), d.scale.code, parts[i].words, d.resolves,
                d.gpu_written, made[i & 7]);
    }
  plume::RenderCommandList *cmd = State().command_list;
  if (!cmd)
    return;
  uint32_t copied = 0;
  for (uint32_t i = 0; i < count; ++i) {
    HostTexture &d = *parts[i].dest;
    HostTexture *source = &d;
    if (parts[i].words) {
      EnsureDepthWordsLocked(d);
      source = d.words.get();
    }
    if (!source || !source->texture || source->host_format != t.host_format || source->scale != t.scale) {
      static uint32_t why_logged = 0;
      if (REXCVAR_GET(tw2de_diag_log) && why_logged++ < 24)
        RGPU_INFO("[tw2de] texture {:08X}: resolve destination {:08X} at row {} not copied: {} (format {} vs {}, "
                  "scale {} vs {}, {}x{}, {} resolves)",
                  t.guest_va, d.guest_va, parts[i].y,
                  !source ? "no words" : !source->texture ? "no texture" : "other format or scale",
                  source ? u32(source->host_format) : 0, u32(t.host_format), source ? source->scale.code : 0,
                  t.scale.code, d.width, d.height, d.resolves);
      continue;
    }
    TransitionTextureLocked(source, plume::RenderTextureLayout::COPY_SOURCE);
    TransitionTextureLocked(&t, plume::RenderTextureLayout::COPY_DEST);
    // tw2de (2026-11-07, dynamic resolution): the part drawn (Coord; the whole without it) to where it is drawn.
    const plume::RenderBox box(0, 0, int32_t(t.scale.DrawnSize(d.width)), int32_t(t.scale.DrawnSize(d.height)));
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get(), 0, 0),
                           plume::RenderTextureCopyLocation::Subresource(source->texture.get()), 0,
                           u32(t.scale.Coord(int32_t(parts[i].y))), 0, &box);
    t.drs_code = d.drs_code;  // tw2de (2026-11-07, dynamic resolution)
    ++copied;
  }
  t.composite_copied = key;
  t.resolve_signature = 0;
  ++g_texture_epoch;
  static uint32_t logged = 0;
  if (REXCVAR_GET(tw2de_diag_log) && logged++ < 40)
    RGPU_INFO("[tw2de] texture {:08X} ({}x{} format {}) put together from {} of {} resolve destinations (frame {})",
              t.guest_va, t.width, t.height, u32(t.guest_format), copied, count, g_frame);
}

// The depth resolve whose words a texture described by 'fetch' reads: a
// 32-bit 8_8_8_8 texture over a depth resolve destination's memory, of its
// size.
HostTexture *DepthWordsSource(const TextureFetch &fetch) {
  if (fetch.Format() != GpuTextureFormat::Fmt8_8_8_8 || g_depth_words.empty())
    return nullptr;
  auto it = g_depth_words.find(fetch.BasePage());
  if (it == g_depth_words.end())
    return nullptr;
  HostTexture *source = it->second;
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  if (!source->words || source->width != width || source->height != height)
    return nullptr;
  return source;
}

// tw2de (tw2de_gpu_depth_words_view, package 28): the slot of a view of the depth resolve's words that 't' (a
// texture over its memory) samples instead of its own copy, or UINT32_MAX (then the copy). The copy is of the whole
// words texture into a texture of the same format, size and one level, made at the first bind after each resolve, and
// every draw binds its textures again: the draws after a resolve read its words either way. So only a texture of that
// format, size and one level, sampled through its own view's description with the fetch's swizzle.
uint32_t WordsViewSlot(HostTexture &t, HostTexture &source, u32 swizzle) {
  if (!tw2de::renut_gpu::DepthWordsViewOn() || !source.words || !source.words->texture ||
      t.host_format != kDepthResolveWordsFormat || t.dimension != TextureDimension::Tex2D || t.stacked ||
      t.mip_levels != 1 || t.alloc_width != source.words->alloc_width || t.alloc_height != source.words->alloc_height)
    return UINT32_MAX;
  for (const HostTexture::WordsView &v : source.words_views) {
    if (v.swizzle == swizzle)
      return v.slot;
  }
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.mipLevels = 1;
  desc.componentMapping = SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format);
  HostTexture::WordsView v;
  v.swizzle = swizzle;
  v.view = source.words->texture->createTextureView(desc);
  if (!v.view)
    return UINT32_MAX;
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    return UINT32_MAX;
  }
  Video::SetTextureSlotLocked(v.slot, source.words->texture.get(), v.view.get());
  ++g_texture_epoch;  // tw2de (CPU proposal 2): a new view
  source.words_views.push_back(std::move(v));
  return source.words_views.back().slot;
}

// Copies the depth resolve's words into 't' (of the source's scale) when it
// has resolved again.
void CopyDepthWords(HostTexture &t, HostTexture &source) {
  if (t.copied_resolve == source.resolves)
    return;
  ++g_texture_epoch;  // tw2de (CPU proposal 2)
  plume::RenderCommandList *cmd = State().command_list;
  TransitionTextureLocked(source.words.get(), plume::RenderTextureLayout::COPY_SOURCE);
  TransitionTextureLocked(&t, plume::RenderTextureLayout::COPY_DEST);
  const plume::RenderBox box(0, 0, int32_t(t.scale.Size(t.width)), int32_t(t.scale.Size(t.height)));
  t.resolve_signature = 0;  // tw2de (tw2de_renut_resolve_skip)
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get(), 0, 0),
                         plume::RenderTextureCopyLocation::Subresource(source.words->texture.get()),
                         0, 0, 0, &box);
  t.copied_resolve = source.resolves;
  t.drs_code = source.drs_code;  // tw2de (2026-11-07, dynamic resolution)
  t.uploaded = true;
}

// Whether a colour resolve destination holds red and blue exchanged (see Create's swap_red_blue), by
// tw2de_resolve_swap_by_swizzle: 1 FF XIII's rule (the fetch swizzle's X is not X); 0 never; 2 as 1 but not swizzle
// 60A; 3 as 1 and every 16_16_16_16; 4 as 2 and every 16_16_16_16.
static bool ResolveSwapsRedBlue(const TextureFetch &fetch) {
  const int32_t mode = REXCVAR_GET(tw2de_resolve_swap_by_swizzle);
  const bool x_not_x = (fetch.Swizzle() & 7) != 0;
  const bool wide = u32(fetch.Format()) == 26;
  switch (mode) {
  case 0: return false;
  case 2: return x_not_x && fetch.Swizzle() != 0x60A;
  case 3: return x_not_x || wide;
  case 4: return (x_not_x && fetch.Swizzle() != 0x60A) || wide;
  default: return x_not_x;
  }
}

// GetResolveTextureLocked; 'fetch' receives the header's fetch constant.
HostTexture *ResolveTarget(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                           const d3d::TextureFetchConstant *header_format, bool depth,
                           TextureFetch &fetch) {
  // tw2de (2026-10-19, render-thread proposal 3): no unconditional g_texture_epoch bump: Create and Retire bump it,
  // and so does a resolve target entry that changes (below).
  if (!texture_va)
    return nullptr;
  if (!header_format) {
    const auto *header = GuestPtr<const d3d::BaseTexture>(texture_va);
    if (!header)
      return nullptr;
    header_format = &header->Format;
  }
  fetch = TextureFetch::FromHeader(*header_format);
  if (!fetch.IsTexture())
    return nullptr;
  u32 desc[6];
  MaskedDesc(fetch, desc);

  auto &slot = TextureSlot(texture_va);
  // tw2de (2026-11-08, tw2de_fsr3_before_post): a destination resolved at two scales in a frame (a 1280x720
  // scene's colour, at its dynamic resolution for the restores after the opaque pass, then upscaled by FSR 3.1 for the
  // post-processing) keeps the other scale's texture aside and swaps them, instead of making one twice a frame.
  if (slot && slot->render_target && slot->texture && slot->host_format == format && slot->scale != scale &&
      std::memcmp(slot->desc, desc, sizeof(desc)) == 0) {
    static std::unordered_map<u32, std::unique_ptr<HostTexture>> spares;
    std::unique_ptr<HostTexture> &spare = spares[texture_va];
    if (spare && spare->texture && spare->host_format == format && spare->scale == scale &&
        std::memcmp(spare->desc, desc, sizeof(desc)) == 0) {
      std::swap(slot, spare);
      ++g_texture_epoch;
    } else {
      if (spare)
        Retire(*spare);
      spare = std::move(slot);
      ++g_texture_epoch;
    }
  }
  if (slot && (!slot->render_target || std::memcmp(slot->desc, desc, sizeof(desc)) != 0 ||
               !slot->texture || slot->host_format != format || slot->scale != scale)) {
    Retire(*slot);
    slot.reset();
  }
  if (!slot) {
    slot = std::make_unique<HostTexture>();
    slot->guest_va = texture_va;
    std::memcpy(slot->desc, desc, sizeof(desc));
    // A depth destination has one component, which the swizzle places.
    if (!Create(*slot, fetch, true, format,
                !depth && ResolveSwapsRedBlue(fetch),
                scale, !depth)) {
      RGPU_ERROR("Resolve destination {:08X}: {}x{} target creation failed", texture_va,
                 slot->width, slot->height);
      Retire(*slot);
      return nullptr;
    }
    if (REXCVAR_GET(tw2de_diag_log))
      RGPU_INFO("[swap] resolve destination {:08X} made: fmt {} swizzle {:03X} host format {}: red and blue {}",
                texture_va, u32(fetch.Format()), fetch.Swizzle(), int(format),
                slot->view_swap_red_blue ? "exchanged" : "as stored");
    // A new render target has no defined content, and D3D12 wants it
    // discarded (or cleared) before a resolve or copy writes part of it.
    if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
      TransitionTextureLocked(slot.get(), plume::RenderTextureLayout::COLOR_WRITE);
      cmd->discardTexture(slot->texture.get());
    }
  }
  slot->gpu_written = true;
  slot->uploaded = true;
  slot->used_frame = g_frame;
  HostTexture *&entry = g_resolve_targets[fetch.BasePage()];
  if (entry != slot.get()) {
    entry = slot.get();
    ++g_texture_epoch;  // tw2de (2026-10-19, render-thread proposal 3): ResolvedAlias's answer changes
  }
  return slot.get();
}

} // namespace

namespace {

// Marks the host textures of the resources unlocked since the last call.
// Unlocks of anything else (vertex buffers, textures never bound) are dropped.
void ApplyUnlocksLocked() {
  std::vector<u32> unlocked;
  {
    std::lock_guard lock(g_unlocked_mutex);
    if (g_unlocked.empty())
      return;
    unlocked.swap(g_unlocked);
  }
  for (u32 va : unlocked) {
    auto it = g_textures.find(va);
    if (it != g_textures.end() && it->second) {
      it->second->unlocked = true;
      it->second->unlock_frame = g_frame;
      it->second->lock_driven = true;
    }
  }
}

// Linear textures are the CPU-written ones. The GPU reads them when it
// executes, after the CPU is done with the frame, so an upload reads memory
// as it is at submission; reading at the draw can catch writes still in
// progress. Only bytes that changed are worth uploading: converting the font
// cache every frame cost more than anything else in Present, and hashing its
// 7 MB every frame still cost more than half a millisecond. How a texture
// changes decides how it is checked, once per frame it is bound:
// - Lock-driven: the game writes it between a Lock and D3D::UnlockResource
//   (the font cache, sub_82B8F848). An unlock uploads; the content is watched
//   for kUnlockWatchFrames after one, and checked every kLinearVerifyFrames
//   for a write that went around the unlock (which ends lock-driven).
// - Dynamic: changed at kDynamicChanges checks in a row (the XMV logo movies,
//   whose decoder writes without a Lock and may still be writing at the
//   draw). Uploaded every frame; checked every kLinearVerifyFrames for having
//   stopped changing.
// - Otherwise the content is hashed at every check.
// A write after the check shows the next frame.
// TW2 (2026-11-13, tw2de_dynamic_texture_rows): the rows of a dynamic linear texture whose pages were written since
// its last upload read them, uploaded alone (read at the next kick's flush, as a whole deferred upload is). The cards
// of its extent were just taken (ExtentClean said written). False: nothing recorded, the caller uploads it whole.
bool UploadDirtyRowsLocked(HostTexture &t, const TextureFetch &fetch) {
  static const bool on = REXCVAR_GET(tw2de_dynamic_texture_rows);
  if (!on || !tw2de::renut_gpu::StoreCardsOn() || tw2de::renut_gpu::StoreCardsVerifyOn() || !t.uploaded ||
      t.read_interval == UINT64_MAX || !t.texture)
    return false;
  if (t.mip_levels != 1 || t.depth != 1 || t.stacked || fetch.Tiled() || fetch.Dimension() != TextureDimension::Tex2D ||
      t.info.decoded_blocks || t.info.block_size != 1 || t.info.host == plume::RenderFormat::UNKNOWN ||
      t.height < 16 || t.alloc_width != t.width || t.alloc_height != t.height)
    return false;
  u32 block_width, block_height;
  FormatBlockSize(fetch.Format(), block_width, block_height);
  const TextureLevelLayout layout = GetTextureLevelLayout(fetch, 0);
  if (block_width != 1 || block_height != 1 || !layout.page || layout.in_tail || layout.pitch_texels < t.width)
    return false;
  ExportUntilePlan export_plan;
  if (PlanExportUntile(fetch, export_plan))
    return false;  // pending memexport results: the whole upload's business
  const uint32_t pitch_bytes = layout.pitch_texels * t.info.guest_block_bytes;
  const uint64_t base = (uint64_t(layout.page) << 12) + layout.offset;
  const uint32_t rows = t.height;
  const uint32_t first_page = u32(base >> 12), last_page = u32((base + uint64_t(rows) * pitch_bytes - 1) >> 12);
  // The runs of rows over written pages; two runs fewer than 4 rows apart are one.
  struct Run {
    uint32_t y0, y1;
  };
  Run runs[16];
  uint32_t run_count = 0, dirty_rows = 0;
  for (uint32_t page = first_page; page <= last_page; ++page) {
    if (!GuestPageWrittenSince(page, t.read_interval))
      continue;
    const uint64_t p0 = uint64_t(page) << 12, p1 = p0 + 4096;
    const uint32_t y0 = p0 > base ? u32((p0 - base) / pitch_bytes) : 0;
    const uint32_t y1 = std::min<uint64_t>(rows, (p1 - base + pitch_bytes - 1) / pitch_bytes);
    if (y1 <= y0)
      continue;
    if (run_count && y0 <= runs[run_count - 1].y1 + 4) {
      if (y1 > runs[run_count - 1].y1) {
        dirty_rows += y1 - std::max(y0, runs[run_count - 1].y1);
        runs[run_count - 1].y1 = y1;
      }
      continue;
    }
    if (run_count == 16)
      return false;
    runs[run_count++] = {y0, y1};
    dirty_rows += y1 - y0;
  }
  if (!run_count || uint64_t(dirty_rows) * 4 > uint64_t(rows) * 3) {
    ++g_row_upload_diag[3];
    return false;  // nothing found on the level's pages (the caller's whole upload decides), or most of it written
  }
  auto &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  const uint32_t row_bytes = t.width * t.info.host_block_bytes;
  const uint32_t row_pitch = (row_bytes + 255u) / 256u * 256u;
  UploadAllocation staging[16];
  for (uint32_t i = 0; i < run_count; ++i) {
    staging[i] = s.uploads[s.frame].Allocate(uint64_t(row_pitch) * (runs[i].y1 - runs[i].y0), 512);
    if (!staging[i])
      return false;  // nothing recorded yet
  }
  // This interval, not the next kick's (which is when the rows are read, and what a whole upload records): the rows
  // left out were only seen clean now. A page written after this and found by another check before the kick (its
  // cards taken in this interval) must count as written at this texture's next check; the price is that the rows
  // uploaded now are uploaded once more then.
  t.read_interval = CardsInterval();
  t.resolve_signature = 0;
  t.export_adopt = false;
  static const uint32_t kProfileLabel = gpu_profile::Label("texture uploads");
  gpu_profile::Mark(s, kProfileLabel);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  t.layout = plume::RenderTextureLayout::COPY_DEST;
  const uint32_t row_texels = row_pitch / t.info.host_block_bytes;
  for (uint32_t i = 0; i < run_count; ++i) {
    const uint32_t n = runs[i].y1 - runs[i].y0;
    LevelSource src;
    src.pitch_blocks = layout.pitch_texels;
    src.height_blocks = layout.height;
    src.x_blocks = layout.tail_x_blocks;
    src.y_blocks = layout.tail_y_blocks + runs[i].y0;
    src.z_offset = layout.tail_z;
    src.base = GuestPhysicalPtr<const uint8_t>(u32(base));
    g_pending_fills.push_back({t.info, fetch, src, t.width, n, 1, static_cast<uint8_t *>(staging[i].cpu), row_pitch,
                               row_pitch * n});
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get(), 0, 0),
                           plume::RenderTextureCopyLocation::PlacedFootprint(staging[i].ref.ref, t.info.host, t.width, n,
                                                                             1, row_texels, staging[i].ref.offset),
                           0, runs[i].y0, 0, nullptr);
  }
  ++g_row_upload_diag[0];
  g_row_upload_diag[1] += dirty_rows;
  g_row_upload_diag[2] += rows;
  if (REXCVAR_GET(tw2de_diag_tex_made))
    TexUploadReport(false);
  return true;
}

void RefreshLinearLocked(HostTexture &t, const TextureFetch &fetch) {
  ApplyUnlocksLocked();
  auto upload = [&](uint64_t hash) {
    t.content_hash = hash;
    t.uploaded = Upload(t, fetch, true);
  };
  const bool verify_due = g_frame - t.verified_frame >= kLinearVerifyFrames;
  // tw2de (2026-10-19, render-thread proposal 1): every hash through CheckedHash (the cards).
  if (!t.uploaded) {
    upload(CheckedHash(fetch, t, false));
    t.verified_frame = g_frame;
  } else if (t.lock_driven) {
    const bool watched = g_frame - t.unlock_frame < kUnlockWatchFrames;
    if (t.unlocked) {
      upload(CheckedHash(fetch, t, false));
    } else if (watched || verify_due) {
      const uint64_t hash = CheckedHash(fetch, t, false);
      if (hash != t.content_hash) {
        upload(hash);
        t.lock_driven = watched;
      }
      if (verify_due)
        t.verified_frame = g_frame;
    }
  } else if (t.dynamic) {
    if (verify_due) {
      const uint64_t hash = CheckedHash(fetch, t, false);
      t.dynamic = hash != t.content_hash;
      t.changes = 0;
      t.content_hash = hash;
      t.verified_frame = g_frame;
    }
    // tw2de (2026-10-19, render-thread proposal 1): not uploaded again while the cards prove no page written since
    // the last upload read it (the GPU's copy holds those bytes).
    KnowGuestExtent(fetch, t);
    if (tw2de::renut_gpu::StoreCardsOn() && !tw2de::renut_gpu::StoreCardsVerifyOn() &&
        ExtentClean(t, t.read_interval)) {
      tw2de::renut_gpu::CountTextureCards(uint64_t(t.sample_sizes[0]) + t.sample_sizes[1], 2);
    } else if (!UploadDirtyRowsLocked(t, fetch)) {  // TW2 (2026-11-13, tw2de_dynamic_texture_rows)
      t.uploaded = Upload(t, fetch, true);
    }
  } else {
    const uint64_t hash = CheckedHash(fetch, t, false);
    if (hash != t.content_hash) {
      upload(hash);
      t.dynamic = ++t.changes >= kDynamicChanges;
      t.verified_frame = g_frame;
    } else {
      t.changes = 0;
    }
  }
  t.unlocked = false;
}

// The backstop (kEvictPressureSlots, kEvictMemoryShare): with the bindless
// table filling up or video memory near its budget, the host textures idle
// for a while go.
void EvictIdleTexturesLocked() {
  const bool slots = Video::TextureSlotsInUseLocked() >= kEvictPressureSlots;
  GpuMemoryUsage memory;
  if (!slots) {
    memory = QueryGpuMemory(State().device.get());
    if (!memory.valid || !memory.local_budget ||
        double(memory.local_used) < double(memory.local_budget) * kEvictMemoryShare) {
      return;
    }
  }
  DropParkedLocked();  // tw2de (2026-11-05): memory or slots short: the parked textures go first
  std::vector<u32> idle;
  const bool keep_gpu_written = tw2de::renut_gpu::KeepGpuWritten();  // tw2de
  for (const auto &[va, t] : g_textures) {
    if (t && t->gpu_written && keep_gpu_written)
      continue;  // tw2de: only the GPU has its content (tw2de_renut_keep_gpu_written)
    const uint64_t limit = t && t->gpu_written ? kEvictIdleGpuFrames : kEvictIdleFrames;
    if (!t || g_frame - t->used_frame >= limit)
      idle.push_back(va);
  }
  for (u32 va : idle) {
    NoteTextureReleased(va, kWhyIdle);  // tw2de (2026-11-05, [texburst])
    ReleaseHostTextureLocked(va);
  }
  if (slots) {
    RGPU_WARN("Textures: bindless table nearly full; {} idle host textures released ({} of {} "
              "slots in use)",
              idle.size(), Video::TextureSlotsInUseLocked(), kTextureDescriptorCount);
  } else if (!idle.empty()) {
    RGPU_WARN("Textures: video memory near its budget ({} of {} MB); {} idle host textures "
              "released",
              memory.local_used >> 20, memory.local_budget >> 20, idle.size());
  }
}

// tw2de (2026-10-17): tw2de_renut_memory_policy (tw2de_renut_tuning.h). Every kColdCheckFrames frames, with less
// than tw2de_renut_textures_reserve_mb left of the GPU memory budget (the driver's, VK_EXT_memory_budget: real
// footprints) or the bindless table three quarters full, the cold host textures go: not bound for kColdFrames frames
// (sa2-nx's kFramesNoUsageForDrop), so no command list still to run reads them (2 frames in flight; a slot given back
// is reused only once its frame has finished, DrainSlot) and a texture in use is never re-uploaded over and over. A
// released texture is made again from guest memory when bound, as it was made the first time. Never one the GPU wrote
// (tw2de_renut_keep_gpu_written). If every texture is warm nothing goes: better a larger cache than thrash. Renut's
// backstop above stays (85 % of the budget, 600 frames), sized for a PC's video memory.
constexpr uint64_t kColdCheckFrames = 32;
constexpr uint64_t kColdFrames = 120;

void EvictColdTexturesLocked(uint64_t reserve) {
  const bool slots = Video::TextureSlotsInUseLocked() >= kEvictPressureSlots;
  uint64_t left = 0, used = 0, budget = 0;
  if (!slots) {
    const GpuMemoryUsage memory = QueryGpuMemory(State().device.get());
    if (!memory.valid)
      return;
    // The device-local heaps where there are any (a PC's video memory); otherwise the shared ones.
    const bool local = memory.local_budget != 0;
    budget = local ? memory.local_budget : memory.shared_budget;
    used = local ? memory.local_used : memory.shared_used;
    if (!budget)
      return;
    left = budget > used ? budget - used : 0;
    if (left >= reserve)
      return;
  }
  DropParkedLocked();  // tw2de (2026-11-05): memory or slots short: the parked textures go first
  const bool keep_gpu_written = tw2de::renut_gpu::KeepGpuWritten();
  std::vector<u32> cold;
  for (const auto &[va, t] : g_textures) {
    if (t && t->gpu_written && keep_gpu_written)
      continue;
    const uint64_t limit = t && t->gpu_written ? kEvictIdleGpuFrames : kColdFrames;
    if (!t || g_frame - t->used_frame >= limit)
      cold.push_back(va);
  }
  for (u32 va : cold) {
    NoteTextureReleased(va, kWhyCold);  // tw2de (2026-11-05, [texburst])
    ReleaseHostTextureLocked(va);
  }
  tw2de::renut_gpu::CountReserveEviction(cold.size());
  static uint32_t logged = 0;
  if (!cold.empty() && logged++ < 32) {
    if (slots) {
      RGPU_WARN("Textures: bindless table three quarters full (tw2de_renut_memory_policy); {} cold host textures "
                "released ({} of {} slots in use)",
                cold.size(), Video::TextureSlotsInUseLocked(), kTextureDescriptorCount);
    } else {
      RGPU_WARN("Textures: {} MB of the GPU memory budget left ({} of {} MB used), under the {} MB reserve "
                "(tw2de_renut_memory_policy); {} cold host textures released",
                left >> 20, used >> 20, budget >> 20, reserve >> 20, cold.size());
    }
  }
}

} // namespace

uint64_t g_texture_frame = 1;  // tw2de (2026-10-19): g_frame, read inline by TextureFrameLocked

uint64_t g_texture_epoch = 1;  // tw2de (2026-10-19, CPU proposal 2)
uint64_t g_depth_epoch = 1;    // tw2de (2026-10-19, render-thread proposal 3)

void AdvanceTextureFrameLocked() {
  TexBurstReportLocked(g_frame);  // tw2de (2026-11-05): the frame just ended
  ++g_frame;
  g_texture_frame = g_frame;
  namespace ft = tw2de::frame_timing;
  {
    ft::PartScope part(ft::kPartTexUnlocks);
    ApplyUnlocksLocked();
  }
  if (g_frame % kEvictCheckFrames == 0) {
    ft::PartScope part(ft::kPartTexIdleEvict);
    EvictIdleTexturesLocked();
  }
  // tw2de: tw2de_renut_memory_policy.
  if (const uint64_t reserve = tw2de::renut_gpu::TextureReserveBytes(); reserve && g_frame % kColdCheckFrames == 0) {
    ft::PartScope part(ft::kPartTexColdEvict);
    EvictColdTexturesLocked(reserve);
  }
}

void ReleaseHostTexturesInMemoryLocked(u32 physical, u32 size) {
  if (!size)
    return;
  const u32 first_page = (physical & 0x1FFFFFFFu) >> 12;
  const u32 end_page = first_page + ((size + 0xFFFu) >> 12);
  auto inside = [&](u32 page) { return page >= first_page && page < end_page; };
  std::vector<u32> gone;
  for (const auto &[va, t] : g_textures) {
    if (!t)
      continue;
    const u32 base_page = t->desc[1] >> 12;
    const u32 mip_page = t->desc[5] >> 12;
    if (inside(base_page) || (mip_page && inside(mip_page)))
      gone.push_back(va);
  }
  for (u32 va : gone) {
    NoteTextureReleased(va, kWhyMemory);  // tw2de (2026-11-05, [texburst])
    ReleaseHostTextureLocked(va);
  }
}

void NoteTextureUnlocked(u32 resource_va) {
  std::lock_guard lock(g_unlocked_mutex);
  g_unlocked.push_back(resource_va);
}

void FlushTextureFillsLocked() {
  const auto start = std::chrono::steady_clock::now();  // tw2de (2026-10-28): TakeTextureUploadReport
  for (const PendingFill &f : g_pending_fills) {
    ReadLevelParallel(f.info, f.fetch, f.src, f.blocks_w, f.blocks_h, f.depth, f.dst, f.row_pitch,
                      f.slice_pitch);
  }
  g_upload_stats.fills += uint32_t(g_pending_fills.size());
  g_tex_fill_diag[0] += g_pending_fills.size();  // TW2 (2026-11-13, tw2de_diag_tex_made)
  g_tex_fill_diag[2] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  g_upload_stats.fill_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  g_pending_fills.clear();
}

HostTexture *FindHostTextureLocked(u32 texture_va) {
  auto it = g_textures.find(texture_va);
  return it != g_textures.end() ? it->second.get() : nullptr;
}

void ReleaseHostTextureLocked(u32 texture_va) {
  auto it = g_textures.find(texture_va);
  if (it == g_textures.end())
    return;
  Retire(*it->second);
  if (TextureMemo &memo = TextureMemoFor(texture_va); memo.va == texture_va)
    memo = {};
  g_textures.erase(it);
}

// tw2de (2026-10-19, tw2de_deko3d_barrier_lean): a layout only read in, both sides: deko3d has no image layouts,
// and two reads need no ordering.
bool ReadOnlyLayout(plume::RenderTextureLayout layout) {
  return layout == plume::RenderTextureLayout::SHADER_READ || layout == plume::RenderTextureLayout::DEPTH_READ ||
         layout == plume::RenderTextureLayout::COPY_SOURCE || layout == plume::RenderTextureLayout::RESOLVE_SOURCE;
}

void TransitionTextureLocked(HostTexture *texture, plume::RenderTextureLayout layout) {
  if (!texture || !texture->texture || texture->layout == layout)
    return;
  if (IsDeko3D() && tw2de::renut_gpu::Deko3DBarrierLeanOn() && ReadOnlyLayout(texture->layout) &&
      ReadOnlyLayout(layout)) {
    texture->layout = layout;
    return;
  }
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  const bool copy = layout == plume::RenderTextureLayout::COPY_DEST ||
                    layout == plume::RenderTextureLayout::COPY_SOURCE ||
                    layout == plume::RenderTextureLayout::RESOLVE_DEST;
  cmd->barriers(copy ? plume::RenderBarrierStage::COPY : plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(texture->texture.get(), layout));
  texture->layout = layout;
}

// tw2de (diagnostic, tw2de_frame_dump): the host texture a draw samples at 'texture_va' read back from the GPU as
// it is (level 0, slice 0) into <dir>/tex_<va>.bin with a text header (format, size, row pitch), once per dump.
// tw2de (2026-10-31): also the frame dump's probe (draw.cpp FrameDumpProbeLocked).
uint32_t DumpBlockBytes(plume::RenderFormat f, uint32_t &block) {
  using F = plume::RenderFormat;
  block = 1;
  switch (f) {
  case F::BC1_UNORM: case F::BC4_UNORM: case F::BC4_SNORM: block = 4; return 8;
  case F::BC2_UNORM: case F::BC3_UNORM: case F::BC5_UNORM: case F::BC5_SNORM: case F::BC7_UNORM: block = 4; return 16;
  case F::R8_UNORM: case F::R8_SNORM: case F::R8_UINT: return 1;
  case F::R8G8_UNORM: case F::R8G8_SNORM: case F::R16_UNORM: case F::R16_FLOAT: case F::R16_SNORM: return 2;
  case F::R8G8B8A8_UNORM: case F::R8G8B8A8_SNORM: case F::B8G8R8A8_UNORM: case F::R16G16_UNORM:
  case F::R16G16_FLOAT: case F::R32_FLOAT: case F::R16G16_SNORM: return 4;
  case F::R16G16B16A16_UNORM: case F::R16G16B16A16_FLOAT: case F::R32G32_FLOAT: case F::R16G16B16A16_SNORM: return 8;
  case F::R10G10B10A2_UNORM: return 4;  // tw2de (2026-10-25, tw2de_deko3d_native_formats)
  case F::R5G6B5_UNORM: case F::R5G5B5A1_UNORM: case F::R4G4B4A4_UNORM: return 2;
  case F::R32G32B32A32_FLOAT: return 16;
  default: return 0;
  }
}

uint64_t TextureFrameForLog() { return g_frame; }

static HostTexture *DumpSourceLocked(u32 texture_va, const TextureFetch &fetch) {
  HostTexture *t = FindHostTextureLocked(texture_va);
  if (!t || !t->texture || (!t->gpu_written && !t->uploaded)) {
    auto it = g_resolve_targets.find(fetch.BasePage());
    if (it != g_resolve_targets.end() && it->second && it->second->texture)
      t = it->second;
  }
  return t;
}

uint64_t DumpContentSignatureLocked(u32 texture_va, const TextureFetch &fetch) {
  const HostTexture *t = DumpSourceLocked(texture_va, fetch);
  // tw2de (2026-10-27, the glitched minimap): the game's art too (uploaded from guest memory), once per frame
  // dump, as the GPU has it: the textures of the draws that show it.
  if (!t || !t->texture || (!t->gpu_written && !t->uploaded))
    return 0;
  return ((uint64_t(uintptr_t(t)) * 0x9E3779B97F4A7C15ull) ^ (t->gpu_written ? t->resolve_signature : t->content_hash)) |
         1u;
}

void DumpTextureLocked(u32 texture_va, const TextureFetch &fetch, const std::string &dir, int draw) {
  HostTexture *t = DumpSourceLocked(texture_va, fetch);
  if (!t || !t->texture || t->dimension != TextureDimension::Tex2D)
    return;
  uint32_t block = 1;
  const uint32_t unit = DumpBlockBytes(t->host_format, block);
  if (!unit)
    return;
  const uint32_t w = std::max(t->scale.Size(t->width), 1u), h = std::max(t->scale.Size(t->height), 1u);
  const uint32_t bw = (w + block - 1) / block, bh = (h + block - 1) / block;
  // rows of 256 bytes (D3D12's placed footprint pitch)
  const uint32_t row_bytes = ((bw * unit) + 255) & ~255u;
  const uint32_t row_texels = row_bytes / unit * block;
  auto &s = State();
  std::unique_ptr<plume::RenderBuffer> readback =
      s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(row_bytes) * bh));
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!readback || !cmd)
    return;
  const plume::RenderTextureLayout before = t->layout;
  TransitionTextureLocked(t, plume::RenderTextureLayout::COPY_SOURCE);
  const plume::RenderBox box(0, 0, int32_t(bw * block), int32_t(bh * block));
  cmd->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), t->host_format, bw * block, bh * block, 1,
                                                        row_texels),
      plume::RenderTextureCopyLocation::Subresource(t->texture.get(), 0, 0), 0, 0, 0, &box);
  detail::SubmitAndWaitLocked(s);
  if (const auto *data = static_cast<const uint8_t *>(readback->map())) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // tw2de (2026-10-22): with the draw that sampled it, as a texture is dumped again whenever a resolve changed it.
    const std::string name = draw >= 0 ? fmt::format("tex_{:08X}_d{:04}.bin", texture_va, draw)
                                       : fmt::format("tex_{:08X}.bin", texture_va);
    std::ofstream out(std::filesystem::path(dir) / name, std::ios::binary);
    const std::string head = fmt::format("RNTEX1 format {} width {} height {} row {} blocks {}x{} unit {} block {} "
                                         "guest_format {} gpu_written {} fetch {:08X} {:08X} {:08X} {:08X} {:08X} "
                                         "{:08X}\n",
                                         u32(t->host_format), w, h, row_bytes, bw, bh, unit, block,
                                         u32(t->guest_format), t->gpu_written, fetch.dword[0], fetch.dword[1],
                                         fetch.dword[2], fetch.dword[3], fetch.dword[4], fetch.dword[5]);
    out.write(head.data(), std::streamsize(head.size()));
    out.write(reinterpret_cast<const char *>(data), std::streamsize(size_t(row_bytes) * bh));
    readback->unmap();
  }
  Video::RetireLocked(std::move(readback));
  // tw2de (2026-10-27, the glitched minimap): a texture made from guest memory is also written as guest memory
  // holds it now (<name>_guest.bin, its base level untiled on the CPU, as Upload reads it), so the dump shows whether
  // the GPU's copy is stale (guest memory changed and the texture was not uploaded again) or the memory is wrong.
  if (!t->gpu_written && t->info.conversion == TexelConversion::Copy && !t->info.decoded_blocks &&
      fetch.Dimension() == TextureDimension::Tex2D && !fetch.Stacked()) {
    const TextureLevelLayout layout = GetTextureLevelLayout(fetch, 0);
    u32 gw = 0, gh = 0, gd = 0;
    fetch.Size(gw, gh, gd);
    u32 block_width = 1, block_height = 1;
    FormatBlockSize(fetch.Format(), block_width, block_height);
    const uint32_t gbs = t->info.block_size;
    const uint32_t gbw = (gw + gbs - 1) / gbs, gbh = (gh + gbs - 1) / gbs;
    const uint32_t grow = ((gbw * t->info.host_block_bytes) + 255) & ~255u;
    if (layout.page && gbw && gbh) {
      LevelSource src;
      src.pitch_blocks = layout.pitch_texels / block_width;
      src.height_blocks = layout.height / block_height;
      src.x_blocks = layout.tail_x_blocks;
      src.y_blocks = layout.tail_y_blocks;
      src.z_offset = layout.tail_z;
      src.base = GuestPhysicalPtr<const uint8_t>((layout.page << 12) + layout.offset);
      if (src.base) {
        std::vector<uint8_t> rows(size_t(grow) * gbh);
        ReadLevel(t->info, fetch, src, gbw, gbh, 1, rows.data(), grow, grow * gbh);
        const std::string gname = draw >= 0 ? fmt::format("tex_{:08X}_d{:04}_guest.bin", texture_va, draw)
                                            : fmt::format("tex_{:08X}_guest.bin", texture_va);
        std::ofstream gout(std::filesystem::path(dir) / gname, std::ios::binary);
        const std::string head = fmt::format(
            "RNTEX1 format {} width {} height {} row {} blocks {}x{} unit {} block {} guest_format {} gpu_written "
            "false source guest page {:05X} content_hash {:016X} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}\n",
            u32(t->host_format), gw, gh, grow, gbw, gbh, t->info.host_block_bytes, gbs, u32(t->guest_format),
            layout.page, t->content_hash, fetch.dword[0], fetch.dword[1], fetch.dword[2], fetch.dword[3],
            fetch.dword[4], fetch.dword[5]);
        gout.write(head.data(), std::streamsize(head.size()));
        gout.write(reinterpret_cast<const char *>(rows.data()), std::streamsize(rows.size()));
      }
    }
  }
  TransitionTextureLocked(t, before == plume::RenderTextureLayout::UNKNOWN ? plume::RenderTextureLayout::SHADER_READ
                                                                            : before);
}

// TW2 (2026-11-13, tw2de_diag_tex_made): the host textures made, by their data's base page.
namespace {
struct TexMadeEntry {
  uint32_t count = 0, headers = 0, last_va = 0, first_va = 0, fmt = 0, w = 0, h = 0, mips = 0, why_mask = 0;
  uint64_t bytes = 0;
};
void TexMadeDiag(u32 texture_va, const TextureFetch &fetch, uint32_t why) {
  static std::unordered_map<uint32_t, TexMadeEntry> made;
  static uint64_t total = 0, by_why[8] = {}, first_frame = 0;
  static auto last = std::chrono::steady_clock::now();
  if (!total)
    first_frame = g_frame;
  TexMadeEntry &e = made[fetch.BasePage()];
  ++e.count;
  ++total;
  ++by_why[why & 7];
  if (e.last_va != texture_va)
    ++e.headers;
  if (!e.first_va)
    e.first_va = texture_va;
  e.last_va = texture_va;
  e.fmt = u32(fetch.Format());
  u32 d;
  fetch.Size(e.w, e.h, d);
  e.mips = fetch.MaxMipLevel() - std::min(fetch.MinMipLevel(), fetch.MaxMipLevel()) + 1;
  e.why_mask |= 1u << why;
  u32 pages[2] = {}, sizes[2] = {};
  TextureGuestExtent(fetch, pages, sizes);
  e.bytes = uint64_t(sizes[0]) + sizes[1];
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10))
    return;
  std::vector<std::pair<uint32_t, TexMadeEntry>> top(made.begin(), made.end());
  std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second.count > b.second.count; });
  uint64_t repeated = 0, repeated_bytes = 0, all_bytes = 0;
  for (const auto &t : top) {
    all_bytes += t.second.bytes * t.second.count;
    if (t.second.count > 1) {
      repeated += t.second.count - 1;
      repeated_bytes += t.second.bytes * (t.second.count - 1);
    }
  }
  std::string line;
  for (size_t i = 0; i < top.size() && i < 16; ++i) {
    const TexMadeEntry &t = top[i].second;
    line += fmt::format(" | base {:05X} x{} ({} headers, first {:08X} last {:08X}) fmt {} {}x{} mips {} {} KB why {:X}",
                        top[i].first, t.count, t.headers, t.first_va, t.last_va, t.fmt, t.w, t.h, t.mips,
                        t.bytes >> 10, t.why_mask);
  }
  RGPU_INFO("[texmade] {} frames: {} host textures made ({:.1f} MB of guest data), {} different data; made again {} "
            "times ({:.1f} MB); by reason: new {} desc {} scale {} idle {} cold {} memory {}{}",
            g_frame - first_frame, total, double(all_bytes) / (1 << 20), made.size(), repeated,
            double(repeated_bytes) / (1 << 20), by_why[0], by_why[1], by_why[2], by_why[3], by_why[4], by_why[5], line);
  made.clear();
  total = 0;
  std::fill(std::begin(by_why), std::end(by_why), 0);
  last = now;
}
}  // namespace

// TW2 (2026-11-14, tw2de_lookahead).
bool SameHostTextureDataLocked(const HostTexture &t, const u32 dwords[6]) {
  TextureFetch fetch;
  std::memcpy(fetch.dword, dwords, sizeof(fetch.dword));
  u32 desc[6];
  MaskedDesc(fetch, desc);
  return SameData(desc, t.desc);
}

TextureBinding BindTextureLocked(u32 texture_va, const TextureFetch &device_fetch, BindEffects *fx) {
  // tw2de (2026-11-06, [texburst]): every bind timed (with the frame timing diagnostic), split by whether it made a
  // host texture: run a267c133's area entry had 25.8 ms of texture time on the render thread, 13.6 of it uploads, 1.8
  // made, 0.5 content checks, the rest not measured.
  struct BindTimer {
    bool on = false;
    uint32_t made_before = 0;
    std::chrono::steady_clock::time_point t0;
    ~BindTimer() {
      if (!on)
        return;
      --g_bind_depth;
      uint32_t made = 0;
      for (uint32_t i = 0; i < kWhyCount; ++i)
        made += g_texburst.made[i];
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (made != made_before) {
        ++g_texburst.binds_made;
        g_texburst.bind_made_ms += ms;
      } else {
        ++g_texburst.binds_kept;
        g_texburst.bind_kept_ms += ms;
      }
    }
  } bind_timer;
  if (g_bind_depth == 0 && tw2de::frame_timing::On()) {
    ++g_bind_depth;
    bind_timer.on = true;
    for (uint32_t i = 0; i < kWhyCount; ++i)
      bind_timer.made_before += g_texburst.made[i];
    bind_timer.t0 = std::chrono::steady_clock::now();
  }
  TextureBinding binding;
  binding.dimension = HostDimension(device_fetch);
  binding.slot = NullSlot(binding.dimension);
  if (!texture_va)
    return binding;
  const auto *header = GuestPtr<const d3d::BaseTexture>(texture_va);
  if (!header)
    return binding;
  TextureFetch fetch = TextureFetch::FromHeader(header->Format);
  // SetTexture copied the header into the device's fetch constant. A header
  // that no longer matches it was a temporary the game has overwritten since
  // (a command buffer runs after its recording thread's stack moved on); the
  // device's copy is what the GPU reads.
  if (!fetch.IsTexture() || fetch.BasePage() != device_fetch.BasePage())
    fetch = device_fetch;
  if (!fetch.IsTexture())
    return binding;

  u32 desc[6];
  MaskedDesc(fetch, desc);
  // tw2de (2026-10-18, the one-frame colour flashes): a header read while the game rewrites it (Ryujinx run 34: a
  // bloom level's header with dwords 3-5 still zero, read as a 1D texture of other data) dropped the resolve's texture
  // and made one from guest memory. The device's copy, taken at SetTexture, is what the GPU reads: it wins whenever the
  // two describe different data.
  if (device_fetch.IsTexture() && std::memcmp(fetch.dword, device_fetch.dword, sizeof(fetch.dword)) != 0) {
    u32 device_desc[6];
    MaskedDesc(device_fetch, device_desc);
    if (!SameData(desc, device_desc)) {
      static uint32_t logged = 0;
      if (logged++ < 16)
        RGPU_INFO("[flash] frame {}: texture {:08X} bound by the device's fetch constant: its header describes other "
                  "data (desc {:08X} {:08X} {:08X} vs the device's {:08X} {:08X} {:08X})",
                  g_frame, texture_va, desc[1], desc[2], desc[5], device_desc[1], device_desc[2], device_desc[5]);
      fetch = device_fetch;
      std::memcpy(desc, device_desc, sizeof(desc));
    } else {
      // tw2de (2026-10-20, the colour flash, tw2de_renut_device_swizzle): the same data, other described fields.
      // SetTexture copies dwords 0, 3 and 5 of the header into the device's fetch constant as they are (signs, pitch,
      // tiling; swizzle, number format, exponent adjust; dimension, packed mips, mip address) and changes only dword
      // 4's mip levels (the sampler's MinMipLevel / MaxMipLevel; d3d_native_state.cpp SetTexture): a header that
      // differs in 0, 3 or 5 is stale. Games build headers as stack temporaries (RenderDoc capture of Ryujinx, frame
      // 5437: a bloom level's header rewritten with the swizzle 1,1,0,0 turned the whole frame yellow); the device's
      // copy is what the GPU reads. The periodic report counts them ("[flash] header vs device", by dword).
      u32 differ = 0;
      for (int i = 0; i < 6; ++i)
        differ |= desc[i] != device_desc[i] ? 1u << i : 0u;
      if (differ) {
        tw2de::renut_gpu::CountHeaderDescDiffer(differ);
        if (fetch.Swizzle() != device_fetch.Swizzle())
          tw2de::renut_gpu::CountDeviceSwizzle(uint32_t(g_frame));
        if (differ & 0x29u) {
          static uint32_t logged = 0;
          if (logged++ < 16)
            RGPU_INFO("[flash] frame {}: texture {:08X} sampled through the device's fetch constant{}: its header (same "
                      "data) is stale: dword0 {:08X}/{:08X} dword3 {:08X}/{:08X} dword5 {:08X}/{:08X} (header/device)",
                      g_frame, texture_va, tw2de::renut_gpu::DeviceSwizzleOn() ? "" : " NOT (key off)", desc[0],
                      device_desc[0], desc[3], device_desc[3], desc[5], device_desc[5]);
          if (tw2de::renut_gpu::DeviceSwizzleOn()) {
            fetch = device_fetch;
            std::memcpy(desc, device_desc, sizeof(desc));
          }
        }
      }
    }
  }
  const u32 swizzle = fetch.Swizzle();
  // tw2de (2026-10-20, SASRT's finding): Renut does not apply the fetch constant's exponent adjust (the result times
  // 2^adjust). Counted, so a log shows whether the game uses it (the "[renut] exponent adjust" line).
  if (const int adjust = fetch.ExpAdjust())
    tw2de::renut_gpu::CountExpAdjust(adjust);
  if (HostTexture *resolved = ResolvedAlias(texture_va, fetch, desc)) {
    resolved->used_frame = g_frame;
    if (resolved->words)
      tw2de::renut_gpu::CountDepthFloatSampled();  // tw2de (2026-10-18): a depth resolve's float target sampled
    TransitionTextureLocked(resolved, plume::RenderTextureLayout::SHADER_READ);
    if (fx) {
      fx->transition[0] = resolved;
      fx->float_sampled = resolved->words != nullptr;
    }
    binding.dimension = resolved->dimension;
    binding.slot = SlotForSwizzle(*resolved, swizzle);
    if (resolved->la_dead || resolved->la_watch)
      LookAheadUsedLocked(*resolved);  // TW2 (2026-11-14, tw2de_lookahead)
    // tw2de (2026-10-19, tw2de_deko3d_depth_handover): its content where it is.
    if (resolved->handed_from || resolved->held_current) {
      const uint32_t handed = DepthHandOverSlotLocked(
          *resolved, SwizzleMapping(swizzle, resolved->view_swap_red_blue, resolved->host_format), fx);
      if (handed != UINT32_MAX)
        binding.slot = handed;
    }
    binding.stacked = resolved->stacked;
    binding.scale = resolved->scale;
    binding.lod_scale = resolved->extra_levels ? resolved->scale : RenderScale{};
    binding.drs_code = resolved->drs_code;  // tw2de (2026-11-07, dynamic resolution)
    return binding;
  }
  auto &slot = TextureSlot(texture_va);
  TexWhy why = kWhyNew;  // tw2de (2026-11-05, [texburst]): why a host texture is made below
  // A resolve destination sampled through a fetch constant that differs from
  // its header only in fields that leave the data alone (the mip range, say)
  // keeps what the resolve wrote.
  if (slot && std::memcmp(slot->desc, desc, sizeof(desc)) != 0 &&
      !(slot->gpu_written && SameData(slot->desc, desc))) {
    // tw2de (2026-10-18, the one-frame colour flashes): a texture only the GPU has the content of, thrown away for a
    // fetch constant that reads the memory otherwise; the new one is filled from guest memory, which the resolve never
    // wrote (zero: green through the game's colour encoding on deko3d; stale on Vulkan).
    if (slot->gpu_written) {
      static uint32_t logged = 0;
      if (logged++ < 64)
        RGPU_WARN("[flash] frame {}: resolve destination {:08X} ({}x{} fmt {}) dropped for a fetch of other data "
                  "(desc {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
                  g_frame, texture_va, slot->width, slot->height, u32(slot->guest_format), slot->desc[0],
                  slot->desc[1], slot->desc[2], slot->desc[3], slot->desc[4], slot->desc[5], desc[0], desc[1], desc[2],
                  desc[3], desc[4], desc[5]);
    }
    if (Parkable(*slot)) {
      ParkLocked(texture_va, slot);  // tw2de (2026-11-05): its content may come back
    } else {
      Retire(*slot);
      slot.reset();
    }
    why = kWhyDesc;
  }
  // What a depth resolve wrote to the memory (only the GPU has it), which is
  // at the resolve's scale; guest data is at the guest size.
  HostTexture *depth_source = !(slot && slot->gpu_written) ? DepthWordsSource(fetch) : nullptr;
  if (depth_source) {
    EnsureDepthWordsLocked(*depth_source);  // tw2de (tw2de_renut_depth_words_lazy)
    if (fx)
      fx->depth_source = true;  // tw2de (2026-10-19, render-thread proposal 3)
  }
  CompositePart composite[8];
  const uint32_t composite_count =
      depth_source || (slot && slot->gpu_written) ? 0 : ResolveComposite(texture_va, fetch, composite, 8);
  const RenderScale scale =
      depth_source ? depth_source->scale : composite_count ? composite[0].dest->scale : RenderScale{};
  if (slot && !slot->gpu_written && slot->scale != scale) {
    Retire(*slot);
    slot.reset();
    why = kWhyScale;
  }
  if (!slot) {
    // tw2de (2026-10-18, the colour flashes): memory a resolve wrote, sampled as other data than the resolve's
    // (ResolvedAlias said no) and not as its depth words: this texture is made from guest memory, which only has what
    // the CPU wrote there.
    if (!depth_source) {
      auto it = g_resolve_targets.find(fetch.BasePage());
      if (it != g_resolve_targets.end() && it->second && it->second->texture) {
        static uint32_t logged = 0;
        const HostTexture &r = *it->second;
        if (logged++ < 64)
          RGPU_WARN("[flash] frame {}: texture {:08X} (format {}) made from guest memory over resolve destination "
                    "{:08X} ({}x{} format {}; desc {:08X} {:08X} {:08X} vs {:08X} {:08X} {:08X})",
                    g_frame, texture_va, u32(fetch.Format()), r.guest_va, r.width, r.height, u32(r.guest_format),
                    r.desc[1], r.desc[2], r.desc[5], desc[1], desc[2], desc[5]);
      }
    }
    slot = std::make_unique<HostTexture>();
    slot->guest_va = texture_va;
    std::memcpy(slot->desc, desc, sizeof(desc));
    // tw2de (diagnostic, the blocky world and scrambled character textures): every kind of texture once, as
    // the fetch describes it, so a log shows which layouts the game uses.
    {
      static std::unordered_map<uint64_t, uint32_t> kinds;
      u32 w, h, d;
      fetch.Size(w, h, d);
      const u32 mips = fetch.MaxMipLevel() - std::min(fetch.MinMipLevel(), fetch.MaxMipLevel()) + 1;
      const uint64_t kind = uint64_t(fetch.Format()) | uint64_t(fetch.Dimension()) << 8 |
                            uint64_t(fetch.Tiled()) << 10 | uint64_t(fetch.PackedMips()) << 11 |
                            uint64_t(fetch.Endian()) << 12 | uint64_t(fetch.Stacked()) << 14 |
                            uint64_t(mips > 1) << 15 | uint64_t(fetch.PitchTexels() != ((w + 31) & ~31u)) << 16 |
                            uint64_t(fetch.MipPage() != 0) << 17 | uint64_t(fetch.Sign(0)) << 18 |
                            uint64_t(fetch.Swizzle()) << 20;
      if (kinds[kind]++ == 0 && kinds.size() <= 200 && REXCVAR_GET(tw2de_diag_log))
        RGPU_INFO("[tw2de] texture kind {}: {:08X} format {} dim {} {}x{}x{} pitch {} tiled {} packed {} endian {} "
                  "stacked {} mips {}-{} mip page {:05X} base {:05X} sign {} swizzle {:03X} exp {}",
                  kinds.size(), texture_va, u32(fetch.Format()), u32(fetch.Dimension()), w, h, d,
                  fetch.PitchTexels(), fetch.Tiled(), fetch.PackedMips(), fetch.Endian(), fetch.Stacked(),
                  fetch.MinMipLevel(), fetch.MaxMipLevel(), fetch.MipPage(), fetch.BasePage(), u32(fetch.Sign(0)),
                  fetch.Swizzle(), fetch.ExpAdjust());
    }
    if (REXCVAR_GET(tw2de_diag_log)) {
      static uint32_t created = 0;
      if (created++ < 4000) {
        u32 pages[2] = {}, sizes[2] = {};
        TextureGuestExtent(fetch, pages, sizes);
        RGPU_INFO("[texnew] frame {}: texture {:08X} base {:05X} fmt {} bytes {:X} mips {:05X}+{:X}", g_frame, texture_va,
                  fetch.BasePage(), u32(fetch.Format()), sizes[0], pages[1], sizes[1]);
      }
    }
    if (why == kWhyNew) {
      if (auto it = g_tex_released.find(texture_va); it != g_tex_released.end()) {
        why = TexWhy(it->second);
        g_tex_released.erase(it);
      }
    }
    ++g_texburst.made[why];
    if (REXCVAR_GET(tw2de_diag_tex_made))  // TW2 (2026-11-13)
      TexMadeDiag(texture_va, fetch, why);
    const auto make_start = std::chrono::steady_clock::now();
    const bool made = Create(*slot, fetch, false, plume::RenderFormat::UNKNOWN, false, scale);
    g_texburst.make_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - make_start).count();
    if (!made) {
      static uint32_t logged = 0;
      if (logged++ < 16) {
        RGPU_WARN("Texture {:08X}: format {} {}x{}x{} not supported", texture_va,
                  u32(fetch.Format()), slot->width, slot->height, slot->depth);
      }
      Retire(*slot);
      return binding;  // keep the entry so it is not retried every draw
    }
  }
  HostTexture &t = *slot;
  if (tw2de::renut_gpu::ResolveTraceOn() && t.gpu_written && t.trace_frame != g_frame) {
    t.trace_frame = g_frame;  // tw2de (package 28): tw2de_renut_diag_resolve_trace
    RGPU_INFO("[trace] sample resolved {:08X} ({}x{} levels {} fmt {})", texture_va, t.width, t.height,
              t.extra_levels ? 1 + t.extra_levels : 1, u32(t.host_format));
  }
  // One that could not be made is not marked used: it goes once idle, and the
  // next bind tries again (the bindless table may have room by then).
  if (!t.texture)
    return binding;
  t.used_frame = g_frame;

  if (depth_source && !t.gpu_written) {
    // tw2de (tw2de_gpu_depth_words_view, package 28): the resolve's words themselves, not a copy.
    if (const uint32_t words_slot = WordsViewSlot(t, *depth_source, swizzle); words_slot != UINT32_MAX) {
      t.copied_resolve = 0;  // its own texture has not got them: copied in full if the view is not used later
      TransitionTextureLocked(depth_source->words.get(), plume::RenderTextureLayout::SHADER_READ);
      tw2de::renut_gpu::CountDepthWords(true);
      if (fx) {
        fx->transition[0] = depth_source->words.get();
        fx->words_viewed = true;
      }
      if (tw2de::renut_gpu::ResolveTraceOn() && t.trace_frame != g_frame) {
        t.trace_frame = g_frame;  // tw2de (2026-10-19): which resolves' words are read, by the view
        RGPU_INFO("[trace] sample depth words (view) {:08X} ({}x{}) from the resolve of {:08X}", texture_va, t.width,
                  t.height, depth_source->guest_va);
      }
      binding.dimension = t.dimension;
      binding.slot = words_slot;
      binding.stacked = false;
      binding.scale = t.scale;
      binding.lod_scale = RenderScale{};
      binding.drs_code = depth_source->drs_code;  // tw2de (2026-11-07, dynamic resolution)
      return binding;
    }
    tw2de::renut_gpu::CountDepthWords(false);
    if (tw2de::renut_gpu::ResolveTraceOn() && t.trace_frame != g_frame) {
      t.trace_frame = g_frame;  // tw2de (2026-10-18): which header samples a depth resolve's words
      RGPU_INFO("[trace] sample depth words {:08X} ({}x{}) from the resolve of {:08X}", texture_va, t.width, t.height,
                depth_source->guest_va);
    }
    CopyDepthWords(t, *depth_source);
  } else if (!t.gpu_written && (t.checked_frame != g_frame || composite_count)) {
    if (t.checked_frame != g_frame) {
      t.checked_frame = g_frame;
      if (!fetch.Tiled()) {
        RefreshLinearLocked(t, fetch);
      } else if (composite_count &&
                 (t.scale.code != RenderScale{}.code || CompositeCoversAll(fetch, t, composite, composite_count))) {
        // tw2de: guest data is of the guest size; the parts are what counts. tw2de (2026-11-05, seen18): and at
        // the guest scale too when the parts cover every row of a one-level texture: the upload from guest memory
        // (which no resolve writes) was overwritten by the parts at once (CopyCompositeLocked below). Run acc7f3ca /
        // 941c2ca4: the shadow cascades' 1024x2048 24_8 texture over its resolve destinations, 7.4 ms of untiling on
        // the CPU at each area entry for nothing.
        t.uploaded = true;
      } else if (ExportTextureOnGpuLocked(fetch)) {
        // tw2de (2026-10-27): its memory has pending memexport results (guest memory does not have them yet):
        // made from their windows on the GPU, and hashed as nothing so that the guest copy is uploaded once the
        // results are there (the cards then change and the hash is taken again).
        t.uploaded = Upload(t, fetch, false);
        t.content_hash = 0;
        t.composite_copied = ~0ull;
        if (WatchedRangeLocked(fetch.BasePage() << 12, 4096))
          RGPU_INFO("[watch] frame {}: texture {:08X} made from memexport windows on the GPU", g_frame, texture_va);
      } else {
        // tw2de (2026-11-06, [texburst]): timed only with the frame timing diagnostic on.
        const bool timed = tw2de::frame_timing::On();
        const auto check_start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        uint64_t hash = CheckedHash(fetch, t, true);  // tw2de (2026-10-19, render-thread proposal 1)
        if (timed) {
          ++g_texburst.checks;
          g_texburst.check_ms +=
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - check_start).count();
        }
        // tw2de (tw2de_texture_full_hash): every byte hashed as well; a texture whose bytes changed while the
        // sampled hash stayed the same is logged and uploaded again.
        if (REXCVAR_GET(tw2de_texture_full_hash)) {
          const uint64_t full = ContentHash(fetch, t);
          if (t.uploaded && hash == t.content_hash && full != t.full_hash) {
            static uint32_t logged = 0;
            if (logged++ < 64)
              RGPU_WARN("[stale] frame {}: texture {:08X} ({}x{} format {}, base {:05X}) changed in guest memory, "
                        "unseen by the sampled hash: uploaded again", g_frame, texture_va, t.width, t.height,
                        u32(fetch.Format()), fetch.BasePage());
            hash ^= full;
          }
          t.full_hash = full;
        }
        // tw2de (2026-11-05): a texture made from memexport windows on the GPU (the export branch above, content
        // hash 0 "so that the guest copy is uploaded once the results are there") already holds those results: their
        // hash is taken as its content's, no upload. Run a707b837: 41 textures (42.6 MB) made from windows in frames
        // 2914-2915 were then uploaded again from guest memory at 2916 in 79 ms, every one with the content its address
        // had held before. Only when every level with pending results was made from a window (PlanExportUntile's
        // 'complete', no fallback): a level read from guest memory before the results came could be stale.
        static const bool adopt_on = !REXCVAR_GET(tw2de_diag_no_export_adopt);
        if (adopt_on && t.export_adopt && t.uploaded && t.content_hash == 0 && hash) {
          t.export_adopt = false;
          t.content_hash = hash;
          ++g_texburst.adopted;
          g_texburst.adopted_bytes += HostTextureBytes(t);
        }
        if (!t.uploaded || hash != t.content_hash) {
          // tw2de (2026-11-05): a parked texture of this address with this content goes back in place of an upload
          // (the slot then holds it; bound again from the top, where its check of this frame is already done).
          if (hash && !REXCVAR_GET(tw2de_texture_full_hash) && TakeParkedLocked(texture_va, slot, desc, hash))
            return BindTextureLocked(texture_va, device_fetch, fx);
          static uint32_t uploads_logged = 0;
          if (t.uploaded && REXCVAR_GET(tw2de_texture_full_hash) && uploads_logged++ < 64)
            RGPU_INFO("[reupload] frame {}: texture {:08X} ({}x{} format {}, base {:05X})", g_frame, texture_va,
                      t.width, t.height, u32(fetch.Format()), fetch.BasePage());
          // tw2de (2026-11-05, [texburst]): a made texture's first upload, or a kept one's changed content; and
          // whether a host texture had this content already.
          const bool first = !t.uploaded;
          t.content_hash = hash;
          t.uploaded = Upload(t, fetch, false);
          t.composite_copied = ~0ull;  // the upload wrote over the parts
          if (tw2de::frame_timing::On() && hash) {
            ++(first ? g_texburst.uploads_new : g_texburst.uploads_changed);
            if (g_tex_contents.size() >= 65536)
              g_tex_contents.clear();
            auto [it, inserted] = g_tex_contents.try_emplace(ContentKey(desc, hash), texture_va);
            if (!inserted) {
              ++(it->second == texture_va ? g_texburst.dup_same : g_texburst.dup_other);
              g_texburst.dup_bytes += HostTextureBytes(t);
              g_texburst.dup_ms += g_last_upload_ms;
              it->second = texture_va;
            }
          }
          if (WatchedRangeLocked(fetch.BasePage() << 12, 4096)) {  // tw2de (2026-10-27, tw2de_diag_watch)
            u32 w = 0, h = 0, d = 0;
            fetch.Size(w, h, d);
            RGPU_INFO("[watch] frame {}: texture {:08X} ({}x{} format {}) uploaded from guest memory, hash {:016X}",
                      g_frame, texture_va, w, h, u32(fetch.Format()), hash);
          }
        }
      }
    }
    if (composite_count) {
      const bool timed = tw2de::frame_timing::On();  // tw2de (2026-11-06, [texburst])
      const auto composite_start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      CopyCompositeLocked(t, composite, composite_count);
      if (timed) {
        ++g_texburst.composites;
        g_texburst.composite_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - composite_start).count();
      }
      if (fx)
        fx->depth_source = true;  // tw2de: bound again after a resolve (g_depth_epoch)
    }
    // tw2de (2026-10-19, the colour flash): a texture neither a resolve nor an upload filled is sampled with what
    // its new image's memory held (deko3d does not clear it).
    if (!t.gpu_written && !t.uploaded) {
      static uint32_t logged = 0;
      if (logged++ < 32)
        RGPU_WARN("[flash] frame {}: texture {:08X} ({}x{} format {}{}) sampled with nothing written to it (the "
                  "upload from guest memory failed)",
                  g_frame, texture_va, t.width, t.height, u32(fetch.Format()), fetch.Tiled() ? ", tiled" : "");
    }
  }
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
  if (t.words)
    tw2de::renut_gpu::CountDepthFloatSampled();  // tw2de (2026-10-18): a depth resolve's float target sampled
  if (fx) {
    fx->transition[0] = &t;
    fx->float_sampled = t.words != nullptr;
  }
  binding.dimension = t.dimension;
  binding.slot = SlotForSwizzle(t, swizzle);
  if (t.la_dead || t.la_watch)
    LookAheadUsedLocked(t);  // TW2 (2026-11-14, tw2de_lookahead)
  // tw2de (2026-10-19, tw2de_deko3d_depth_handover): its content where it is.
  if (t.handed_from || t.held_current) {
    const uint32_t handed = DepthHandOverSlotLocked(
        t, SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format), fx);
    if (handed != UINT32_MAX)
      binding.slot = handed;
  }
  binding.stacked = t.stacked;
  binding.scale = t.scale;
  binding.lod_scale = t.extra_levels ? t.scale : RenderScale{};
  binding.drs_code = t.drs_code;  // tw2de (2026-11-07, dynamic resolution)
  return binding;
}

void TextureGuestExtent(const TextureFetch &fetch, uint32_t pages[2], uint32_t sizes[2]) {
  // Memo over the fetch constant: while memexport results are pending, every
  // draw asks for its textures' extents (DrawNeedsExportFlush), and the layout
  // math cost more than the rest of the check.
  struct Entry {
    uint32_t key[6];
    uint32_t pages[2];
    uint32_t sizes[2];
    bool valid;
  };
  static_assert(sizeof(fetch.dword) == sizeof(Entry::key));
  // tw2de (2026-10-19, render-thread proposal 4): a plain static table of 256 (12 KB): both callers run on the render
  // thread (the draw's export check, KnowGuestExtent), and the thread-local one (48 KB, more than the A57's L1) cost a
  // call to read the thread pointer at every access (-mtp=soft).
  static Entry memo[256] = {};
  uint32_t key[6];
  std::memcpy(key, fetch.dword, sizeof(key));
  uint32_t hash = 0;
  for (uint32_t word : key)
    hash = (hash ^ word) * 0x9E3779B1u;
  Entry &entry = memo[hash >> 24];
  if (entry.valid && std::memcmp(entry.key, key, sizeof(key)) == 0) {
    std::memcpy(pages, entry.pages, sizeof(entry.pages));
    std::memcpy(sizes, entry.sizes, sizeof(entry.sizes));
    return;
  }

  uint32_t base_bytes, mip_bytes;
  GetTextureDataSizes(fetch, base_bytes, mip_bytes);
  pages[0] = fetch.BasePage();
  sizes[0] = base_bytes;
  pages[1] = mip_bytes ? fetch.MipPage() : 0;
  sizes[1] = mip_bytes;
  std::memcpy(entry.key, key, sizeof(key));
  std::memcpy(entry.pages, pages, sizeof(entry.pages));
  std::memcpy(entry.sizes, sizes, sizeof(entry.sizes));
  entry.valid = true;
}

HostTexture *GetResolveTextureLocked(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                                     const d3d::TextureFetchConstant *header_format) {
  TextureFetch fetch;
  HostTexture *t = ResolveTarget(texture_va, format, scale, header_format, false, fetch);
  if (t) {
    ++t->resolves;  // tw2de: ResolveComposite copies again when it changes
    ++g_depth_epoch;  // tw2de: and a draw's memoised bindings of a composite (BindEffects::depth_source) go
  }
  return t;
}

HostTexture *GetDepthResolveTextureLocked(u32 texture_va, RenderScale scale,
                                          const d3d::TextureFetchConstant *header_format) {
  TextureFetch fetch;
  HostTexture *t =
      ResolveTarget(texture_va, kDepthResolveFormat, scale, header_format, true, fetch);
  if (!t)
    return nullptr;
  if (!t->words) {
    auto words = std::make_unique<HostTexture>();
    words->width = t->width;
    words->height = t->height;
    words->scale = t->scale;
    words->alloc_width = t->alloc_width;
    words->alloc_height = t->alloc_height;
    words->host_format = kDepthResolveWordsFormat;
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
        t->alloc_width, t->alloc_height, 1, kDepthResolveWordsFormat);
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
    plume::RenderDevice *device = Video::Device();
    words->texture = device->createTexture(desc);
    if (!TextureCreated(words->texture.get())) {
      ReportDeviceLost(device);
      return nullptr;
    }
    words->texture->setName("renut depth resolve words");
    const plume::RenderTexture *targets[] = {t->texture.get(), words->texture.get()};
    t->depth_framebuffer = device->createFramebuffer(plume::RenderFramebufferDesc(targets, 2));
    if (!t->depth_framebuffer) {
      Video::RetireLocked(std::move(words->texture));
      return nullptr;
    }
    if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
      TransitionTextureLocked(words.get(), plume::RenderTextureLayout::COLOR_WRITE);
      cmd->discardTexture(words->texture.get());
    }
    t->words = std::move(words);
  }
  HostTexture *&words_entry = g_depth_words[fetch.BasePage()];
  if (words_entry != t) {
    words_entry = t;
    ++g_texture_epoch;  // tw2de (2026-10-19, render-thread proposal 3): DepthWordsSource's answer changes
  }
  ++g_depth_epoch;  // tw2de (2026-10-19): the words of this destination may change with this resolve
  ++t->resolves;
  return t;
}

namespace {

// Single-level color targets the host levels are drawn into (plume's barriers
// cover a whole texture, so a level cannot be drawn while level 0 of the same
// texture is sampled) and multisampled resolves staged in: a bare target
// (texture, layout, size) and its framebuffer, by format and size. Kept for
// reuse.
struct ScratchTarget {
  HostTexture target;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  // Its bindless slot, for the next level down to read (ScratchSlot).
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = UINT32_MAX;
};

uint32_t ScratchSlot(ScratchTarget &scratch) {
  if (scratch.slot != UINT32_MAX)
    return scratch.slot;
  scratch.view = scratch.target.texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(scratch.target.host_format));
  if (!scratch.view)
    return kInvalidDescriptor;
  scratch.slot = Video::AllocateTextureSlotLocked();
  if (scratch.slot != kInvalidDescriptor)
    Video::SetTextureSlotLocked(scratch.slot, scratch.target.texture.get(), scratch.view.get());
  return scratch.slot;
}
std::unordered_map<uint64_t, std::unique_ptr<ScratchTarget>> g_scratch_targets;
// The level pipelines, by render target format.
std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> g_mip_pipelines;

ScratchTarget *GetScratchTarget(plume::RenderFormat format, uint32_t width, uint32_t height) {
  const uint64_t key = uint64_t(format) << 40 | uint64_t(width) << 20 | height;
  auto &entry = g_scratch_targets[key];
  if (entry)
    return entry.get();
  auto scratch = std::make_unique<ScratchTarget>();
  HostTexture &t = scratch->target;
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, format);
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  plume::RenderDevice *device = Video::Device();
  t.texture = device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    ReportDeviceLost(device);
    return nullptr;
  }
  t.texture->setName("renut scratch target");
  t.host_format = format;
  t.width = t.alloc_width = width;
  t.height = t.alloc_height = height;
  const plume::RenderTexture *targets[] = {t.texture.get()};
  scratch->framebuffer = device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
  if (!scratch->framebuffer) {
    Video::RetireLocked(std::move(t.texture));
    return nullptr;
  }
  // Like a resolve destination, defined before anything writes part of it.
  if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
    TransitionTextureLocked(&t, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->discardTexture(t.texture.get());
  }
  entry = std::move(scratch);
  return entry.get();
}

plume::RenderPipeline *MipPipeline(plume::RenderFormat format) {
  auto &pipeline = g_mip_pipelines[uint32_t(format)];
  if (pipeline)
    return pipeline.get();
  auto &s = State();
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.utility_layout.get();
  desc.vertexShader = s.present_vs.get();
  desc.pixelShader = s.texture_mip_ps.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  pipeline = s.device->createGraphicsPipeline(desc);
  if (!pipeline) {
    RGPU_ERROR("Texture mip pipeline creation failed (format {})", uint32_t(format));
    return nullptr;
  }
  pipeline->setName("renut texture mip");
  return pipeline.get();
}

// Level 0 of 't' without the fetch swizzle, in the texture table.
uint32_t Level0Slot(HostTexture &t) {
  if (t.level0_slot != UINT32_MAX)
    return t.level0_slot;
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.mipSlice = 0;
  desc.mipLevels = 1;
  desc.componentMapping = plume::RenderComponentMapping();
  t.level0_view = t.texture->createTextureView(desc);
  if (!t.level0_view)
    return kInvalidDescriptor;
  t.level0_slot = Video::AllocateTextureSlotLocked();
  if (t.level0_slot != kInvalidDescriptor)
    Video::SetTextureSlotLocked(t.level0_slot, t.texture.get(), t.level0_view.get());
  return t.level0_slot;
}

} // namespace

HostTexture *ResolveStagingLocked(const HostTexture *dest) {
  ScratchTarget *scratch = GetScratchTarget(dest->host_format, dest->alloc_width, dest->alloc_height);
  return scratch ? &scratch->target : nullptr;
}

// tw2de (2026-11-08): the levels drawn straight into the texture's own levels (HostTexture::MipTarget): each level a
// target of a view of that level, read by the next one through a sampled view of it. False: not possible (the scratch
// path is used).
bool UpdateResolveMipsDirectLocked(HostTexture &t, const plume::RenderRect &rect, plume::RenderPipeline *pipeline,
                                   uint32_t source_slot) {
  auto &s = State();
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  const uint32_t count = std::min(t.extra_levels, uint32_t(kMaxExtraLevels));
  if (!cmd || !count || !t.render_target)
    return false;
  if (t.mip_targets.size() < count + 1)
    t.mip_targets.resize(count + 1);
  for (uint32_t level = 1; level <= count; ++level) {
    HostTexture::MipTarget &m = t.mip_targets[level];
    if (m.framebuffer)
      continue;
    plume::RenderTextureViewDesc desc = t.view_desc;
    desc.mipSlice = level;
    desc.mipLevels = 1;
    desc.componentMapping = plume::RenderComponentMapping();
    m.view = t.texture->createTextureView(desc);
    if (!m.view)
      return false;
    const plume::RenderTexture *targets[] = {t.texture.get()};
    const plume::RenderTextureView *views[] = {m.view.get()};
    plume::RenderFramebufferDesc fd(targets, 1);
    fd.colorAttachmentViews = views;
    m.framebuffer = s.device->createFramebuffer(fd);
    if (!m.framebuffer)
      return false;
    m.slot = level < count ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
    if (m.slot != kInvalidDescriptor && m.slot != UINT32_MAX)
      Video::SetTextureSlotLocked(m.slot, t.texture.get(), m.view.get());
  }
  // Level 0 written (the resolve) before it is read.
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  const int32_t align = 1 << count;
  const plume::RenderRect aligned(rect.left & ~(align - 1), rect.top & ~(align - 1),
                                  (rect.right + align - 1) & ~(align - 1), (rect.bottom + align - 1) & ~(align - 1));
  uint32_t source = source_slot;
  for (uint32_t level = 1; level <= count; ++level) {
    const int32_t span = 1 << level;
    const uint32_t width = std::max(1u, t.alloc_width >> level);
    const uint32_t height = std::max(1u, t.alloc_height >> level);
    const plume::RenderRect region(std::min(aligned.left >> level, int32_t(width)),
                                   std::min(aligned.top >> level, int32_t(height)),
                                   std::min((aligned.right + span - 1) >> level, int32_t(width)),
                                   std::min((aligned.bottom + span - 1) >> level, int32_t(height)));
    if (region.right <= region.left || region.bottom <= region.top || source == kInvalidDescriptor)
      break;
    if (level > 1)  // the level above, drawn just now, is read
      cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                    plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    cmd->setFramebuffer(t.mip_targets[level].framebuffer.get());
    cmd->setGraphicsPipelineLayout(s.utility_layout.get());
    cmd->setPipeline(pipeline);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
    cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
    cmd->setScissors(region);
    UtilityPush push{};
    push.scale[0] = push.scale[1] = 1.0f;
    push.texture_index = source;
    push.sampler_index = kPointClampSamplerSlot;
    push.param0 = 2.0f;
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    cmd->drawInstanced(3, 1, 0, 0);
    source = t.mip_targets[level].slot;
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  t.layout = plume::RenderTextureLayout::SHADER_READ;
  return true;  // (the callers bind their own targets, layout and pipeline again, as after the scratch path)
}

namespace {
void MakeResolveMipsLocked(HostTexture *texture, const plume::RenderRect &rect);
}  // namespace

uint32_t PendingMipsLocked() { return g_mips_pending; }

void UpdateResolveMipsLocked(HostTexture *texture, const plume::RenderRect &rect) {
  static const bool lazy = REXCVAR_GET(tw2de_lazy_resolve_mips);
  if (!texture || !texture->extra_levels)
    return;
  if (!lazy || !IsDeko3D()) {
    MakeResolveMipsLocked(texture, rect);
    return;
  }
  HostTexture &t = *texture;
  if (!t.mips_pending_set) {
    t.mips_pending = rect;
    t.mips_pending_set = true;
    ++g_mips_pending;
  } else {
    t.mips_pending.left = std::min(t.mips_pending.left, rect.left);
    t.mips_pending.top = std::min(t.mips_pending.top, rect.top);
    t.mips_pending.right = std::max(t.mips_pending.right, rect.right);
    t.mips_pending.bottom = std::max(t.mips_pending.bottom, rect.bottom);
  }
}

bool MakePendingMipsLocked(u32 texture_va, uint32_t target_width) {
  if (!g_mips_pending || !texture_va || !target_width)
    return false;
  const auto it = g_textures.find(texture_va);
  if (it == g_textures.end() || !it->second)
    return false;
  HostTexture &t = *it->second;
  if (!t.mips_pending_set || target_width * 5 > t.alloc_width * 4)
    return false;  // nothing waiting, or sampled at (about) its size
  t.mips_pending_set = false;
  --g_mips_pending;
  MakeResolveMipsLocked(&t, t.mips_pending);
  return true;
}

namespace {
void MakeResolveMipsLocked(HostTexture *texture, const plume::RenderRect &rect) {
  auto &s = State();
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!texture || !texture->texture || !texture->extra_levels || !cmd || !s.texture_mip_ps)
    return;
  HostTexture &t = *texture;
  plume::RenderPipeline *pipeline = MipPipeline(t.host_format);
  const uint32_t source_slot = Level0Slot(t);
  if (!pipeline || source_slot == kInvalidDescriptor)
    return;
  static const uint32_t kProfileLabel = gpu_profile::Label("resolve host mip levels");
  gpu_profile::Mark(s, kProfileLabel);
  if (IsDeko3D() && UpdateResolveMipsDirectLocked(t, rect, pipeline, source_slot))
    return;

  // Each level is the 2x2 box of the one above: level 1 of level 0, the rest
  // of the previous level's scratch target. The rect is first widened to
  // whole texels of the smallest level, so each level's region is exactly
  // what the next one reads (the scratch targets are shared, and hold other
  // textures' texels outside what was drawn now).
  struct Level {
    ScratchTarget *scratch;
    plume::RenderRect region;
  };
  Level levels[kMaxExtraLevels] = {};
  const uint32_t count = std::min(t.extra_levels, uint32_t(std::size(levels)));
  const int32_t align = 1 << count;
  const plume::RenderRect aligned(rect.left & ~(align - 1), rect.top & ~(align - 1),
                                  (rect.right + align - 1) & ~(align - 1),
                                  (rect.bottom + align - 1) & ~(align - 1));
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
  uint32_t source = source_slot;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t level = i + 1;
    const int32_t span = 1 << level;
    const uint32_t width = std::max(1u, t.alloc_width >> level);
    const uint32_t height = std::max(1u, t.alloc_height >> level);
    Level &l = levels[i];
    l.region = plume::RenderRect(std::min(aligned.left >> level, int32_t(width)),
                                 std::min(aligned.top >> level, int32_t(height)),
                                 std::min((aligned.right + span - 1) >> level, int32_t(width)),
                                 std::min((aligned.bottom + span - 1) >> level, int32_t(height)));
    if (l.region.right <= l.region.left || l.region.bottom <= l.region.top)
      break;
    l.scratch = GetScratchTarget(t.host_format, width, height);
    if (!l.scratch || source == kInvalidDescriptor) {
      l.scratch = nullptr;
      break;
    }
    if (i > 0) {
      // The level above, drawn just now, is read.
      TransitionTextureLocked(&levels[i - 1].scratch->target, plume::RenderTextureLayout::SHADER_READ);
    }
    TransitionTextureLocked(&l.scratch->target, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->setFramebuffer(l.scratch->framebuffer.get());
    cmd->setGraphicsPipelineLayout(s.utility_layout.get());
    cmd->setPipeline(pipeline);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
    // Pixel coordinates are texels of the level.
    cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
    cmd->setScissors(l.region);
    UtilityPush push{};
    push.scale[0] = push.scale[1] = 1.0f;
    push.texture_index = source;
    push.sampler_index = kPointClampSamplerSlot;
    push.param0 = 2.0f;
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    cmd->drawInstanced(3, 1, 0, 0);
    source = ScratchSlot(*l.scratch);
  }

  // tw2de (2026-11-08): one barrier for the texture and every scratch level (a copy barrier is a full one on deko3d:
  // one per level kept the GPU idle between the copies; "resolve host mip levels" was 9.3 ms a frame with the
  // post-processing at 1920x1080).
  {
    plume::RenderTextureBarrier barriers[kMaxExtraLevels + 1];
    uint32_t n = 0;
    if (t.layout != plume::RenderTextureLayout::COPY_DEST) {
      barriers[n++] = plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_DEST);
      t.layout = plume::RenderTextureLayout::COPY_DEST;
    }
    for (uint32_t i = 0; i < count; ++i) {
      HostTexture *scratch = levels[i].scratch ? &levels[i].scratch->target : nullptr;
      if (scratch && scratch->layout != plume::RenderTextureLayout::COPY_SOURCE) {
        barriers[n++] = plume::RenderTextureBarrier(scratch->texture.get(), plume::RenderTextureLayout::COPY_SOURCE);
        scratch->layout = plume::RenderTextureLayout::COPY_SOURCE;
      }
    }
    if (n)
      cmd->barriers(plume::RenderBarrierStage::COPY, barriers, n);
  }
  {
    // tw2de (2026-11-08, diagnostic with renut_gpu_profile): how many level chains a frame, of which sizes.
    static uint64_t calls = 0, levels_made = 0;
    static std::map<std::pair<uint32_t, uint32_t>, uint32_t> sizes;
    static auto last = std::chrono::steady_clock::now();
    static uint64_t last_frame = g_frame;
    ++calls;
    levels_made += count;
    ++sizes[{t.alloc_width, t.alloc_height}];
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(10)) {
      const uint64_t frames = std::max<uint64_t>(g_frame - last_frame, 1);
      std::string list;
      for (const auto &[wh, c] : sizes)
        list += fmt::format(" {}x{}: {:.1f}", wh.first, wh.second, double(c) / double(frames));
      static const bool report = REXCVAR_GET(renut_gpu_profile) > 0.0;
      if (report)
        RGPU_INFO("[mips] {:.1f} level chains a frame ({:.1f} levels), by texture size:{}",
                  double(calls) / double(frames), double(levels_made) / double(frames), list);
      calls = levels_made = 0;
      sizes.clear();
      last = now;
      last_frame = g_frame;
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    const Level &l = levels[i];
    if (!l.scratch)
      continue;
    const plume::RenderBox box(l.region.left, l.region.top, l.region.right, l.region.bottom);
    cmd->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(t.texture.get(), i + 1, 0),
        plume::RenderTextureCopyLocation::Subresource(l.scratch->target.texture.get()),
        uint32_t(l.region.left), uint32_t(l.region.top), 0, &box);
  }
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
}

}  // namespace

namespace {

// The host texture's levels in bytes, from its host format.
uint64_t HostTextureBytes(const HostTexture &t) {
  if (!t.texture)
    return 0;
  const uint32_t block = std::max(plume::RenderFormatBlockWidth(t.host_format), 1u);
  const uint64_t block_bytes = plume::RenderFormatSize(t.host_format);
  const uint32_t layers = t.stacked ? StackedLayers(t) : t.depth;
  uint64_t bytes = 0;
  for (uint32_t level = 0; level < t.mip_levels; ++level) {
    const uint64_t w = std::max(1u, t.alloc_width >> level);
    const uint64_t h = std::max(1u, t.alloc_height >> level);
    const uint64_t d = t.dimension == TextureDimension::Tex3D && !t.stacked
                           ? std::max(1u, t.depth >> level)
                           : layers;
    bytes += (w + block - 1) / block * ((h + block - 1) / block) * block_bytes * d;
  }
  if (t.words)
    bytes += HostTextureBytes(*t.words);
  return bytes;
}

} // namespace

TextureMemory TextureMemoryLocked() {
  TextureMemory usage;
  for (const auto &[va, t] : g_textures) {
    if (!t || !t->texture)
      continue;
    const uint64_t bytes = HostTextureBytes(*t);
    ++usage.textures;
    usage.bytes += bytes;
    if (t->render_target) {
      ++usage.resolve_targets;
      usage.resolve_bytes += bytes;
    }
  }
  return usage;
}

namespace {
uint32_t SamplerSlotUncached(const TextureFetch &fetch, RenderScale lod_scale);
} // namespace

uint32_t SamplerSlotLocked(const TextureFetch &fetch, RenderScale lod_scale) {
  // Every draw asks for each of its textures: remember the answer by the
  // fetch constant words the sampler state comes from (clamps in 0, filters,
  // anisotropy and LOD bias in 3, mip range in 4, border color in 5).
  struct Memo {
    bool valid = false;
    u32 words[4] = {};
    RenderScale lod_scale;
    uint32_t min_anisotropy = 1;
    uint32_t slot = 0;
  };
  static Memo memo[256];
  const uint32_t min_anisotropy = MinAnisotropy();
  const u32 words[4] = {fetch.dword[0], fetch.dword[3], fetch.dword[4], fetch.dword[5]};
  Memo &entry = memo[(words[0] * 0x9E3779B1u ^ words[1] * 0x85EBCA6Bu ^ words[2] * 0xC2B2AE35u ^
                      words[3] ^ lod_scale.code * 0x27D4EB2Fu) >> 24];
  if (entry.valid && entry.lod_scale == lod_scale && entry.min_anisotropy == min_anisotropy &&
      std::memcmp(entry.words, words, sizeof(words)) == 0)
    return entry.slot;
  const uint32_t slot = SamplerSlotUncached(fetch, lod_scale);
  entry.valid = true;
  std::memcpy(entry.words, words, sizeof(words));
  entry.lod_scale = lod_scale;
  entry.min_anisotropy = min_anisotropy;
  entry.slot = slot;
  return slot;
}

namespace {

// The sampler slot for the fetch constant's sampler state, created the first
// time it is asked for.
uint32_t SamplerSlotUncached(const TextureFetch &fetch, RenderScale lod_scale) {
  auto address = [](TextureAddress mode) {
    switch (mode) {
    case TextureAddress::Wrap:
      return plume::RenderTextureAddressMode::WRAP;
    case TextureAddress::Mirror:
      return plume::RenderTextureAddressMode::MIRROR;
    case TextureAddress::Border:
    case TextureAddress::MirrorOnceBorder:
      return plume::RenderTextureAddressMode::BORDER;
    case TextureAddress::MirrorOnce:
    case TextureAddress::MirrorOnceBorderHalf:
      return plume::RenderTextureAddressMode::MIRROR_ONCE;
    default:
      return plume::RenderTextureAddressMode::CLAMP;
    }
  };
  auto filter = [](TextureFilter f) {
    return f == TextureFilter::Point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  };

  plume::RenderSamplerDesc desc;
  desc.addressU = address(fetch.Address(0));
  desc.addressV = address(fetch.Address(1));
  desc.addressW = address(fetch.Address(2));
  desc.magFilter = filter(fetch.MagFilter());
  desc.minFilter = filter(fetch.MinFilter());
  desc.mipmapMode = fetch.MipFilter() == TextureFilter::Point ? plume::RenderMipmapMode::NEAREST
                                                              : plume::RenderMipmapMode::LINEAR;
  // MaxAnisotropy stores 2..5 for ratios 2:1..16:1 (its table at 0x82003E38).
  const uint32_t aniso = fetch.Anisotropy();
  desc.anisotropyEnabled = aniso > 1 && aniso <= 5;
  desc.maxAnisotropy = desc.anisotropyEnabled ? (1u << (aniso - 1)) : 1;
  // renut_anisotropic_filtering: the least a mipmapped, linearly filtered
  // texture gets (render targets' host levels keep their own filtering).
  const uint32_t min_anisotropy = MinAnisotropy();
  if (min_anisotropy > desc.maxAnisotropy && lod_scale.code == 0 &&
      desc.minFilter == plume::RenderFilter::LINEAR &&
      desc.magFilter == plume::RenderFilter::LINEAR &&
      fetch.MipFilter() != TextureFilter::BaseMap && fetch.MaxMipLevel() > fetch.MinMipLevel()) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = min_anisotropy;
  }
  desc.minLOD = float(fetch.MinMipLevel());
  desc.maxLOD = fetch.MipFilter() == TextureFilter::BaseMap ? float(fetch.MinMipLevel())
                                                            : float(fetch.MaxMipLevel());
  desc.mipLODBias = float(fetch.LodBias()) / 32.0f;  // MipMapLodBias stores bias * 32
  // A resolve destination with host levels (HostTexture::extra_levels) has
  // the guest's one level: a filtered fetch reaches the host level whose
  // texel is the guest's, blending between them; a point fetch stays at the
  // guest texel it hits.
  if (lod_scale.Factor() > 1.0f) {
    desc.minLOD = 0.0f;
    if (desc.minFilter == plume::RenderFilter::LINEAR) {
      desc.maxLOD = std::log2(lod_scale.Factor());
      desc.mipmapMode = plume::RenderMipmapMode::LINEAR;
    } else {
      desc.maxLOD = 0.0f;
    }
  }
  // BorderColor stores 0 for a zero colour and 1 (white) for any other.
  switch (fetch.BorderColor()) {
  case 1:
    desc.borderColor = plume::RenderBorderColor::OPAQUE_WHITE;
    break;
  case 0:
    desc.borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
    break;
  default:
    desc.borderColor = plume::RenderBorderColor::OPAQUE_BLACK;
    break;
  }

  uint64_t key = uint64_t(desc.addressU) | uint64_t(desc.addressV) << 4 |
                 uint64_t(desc.addressW) << 8 | uint64_t(desc.magFilter) << 12 |
                 uint64_t(desc.minFilter) << 14 | uint64_t(desc.mipmapMode) << 16 |
                 uint64_t(desc.maxAnisotropy) << 18 | uint64_t(fetch.MinMipLevel()) << 24 |
                 uint64_t(fetch.MaxMipLevel()) << 28 | uint64_t(fetch.MipFilter()) << 32 |
                 uint64_t(uint32_t(fetch.LodBias()) & 0x3FF) << 34 | uint64_t(desc.borderColor) << 44 |
                 uint64_t(lod_scale.code) << 48;
  auto it = g_samplers.find(key);
  if (it != g_samplers.end())
    return it->second;

  auto &s = State();
  uint32_t slot = UINT32_MAX;
  for (uint32_t i = kFirstDynamicSamplerSlot; i < kSamplerDescriptorCount; ++i) {
    if (!s.sampler_slot_used[i]) {
      slot = i;
      break;
    }
  }
  if (slot == UINT32_MAX) {
    static bool logged = false;
    if (!logged) {
      RGPU_WARN("Sampler table full; falling back to linear clamp");
      logged = true;
    }
    return kLinearClampSamplerSlot;
  }
  auto sampler = s.device->createSampler(desc);
  if (!sampler)
    return kLinearClampSamplerSlot;
  s.sampler_slot_used[slot] = true;
  s.sampler_set->setSampler(slot, sampler.get());
  g_sampler_objects.push_back(std::move(sampler));
  g_samplers.emplace(key, slot);
  return slot;
}

} // namespace

} // namespace renut::gpu

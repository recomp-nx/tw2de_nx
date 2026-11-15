/**
 * @file    gpu/render/draw.h
 * @brief   Guest draws, clears and resolves, recorded from the D3D device's
 *          state at the time of the call.
 *
 * The hooked D3D entry points hand over the call's arguments; everything else
 * (bound shaders, declaration, streams, textures, render targets, render state
 * and constants) is read from a snapshot of the guest D3DDevice, which the
 * game's Set* calls and inline state writes keep current: taken at the call
 * (queued.h) or when a command buffer recorded it. These run on the render
 * thread (render_thread.h) and require State().mutex.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/backend/video.h"

namespace renut::gpu {

// tw2de (2026-10-19, render-thread proposal 1, tw2de_renut_store_cards): whether no page of the guest range at
// 'data' (physical memory, through any of its views) was written since interval 'proven' (CardsInterval at the time),
// reading and clearing its cards as the vertex copies' proof does; and the current interval (one per kick).
bool GuestRangeCardsClean(const u8 *data, size_t bytes, uint64_t proven);
uint64_t CardsInterval();
// TW2 (2026-11-13, tw2de_dynamic_texture_rows): after GuestRangeCardsClean has taken a range's cards, whether its
// physical page 'page' was found written in or after interval 'proven' (or the page before it: a store marks its first
// byte's page only).
bool GuestPageWrittenSince(u32 page, uint64_t proven);

struct DrawCall {
  u32 primitive = 0;  // D3DPRIMITIVETYPE (Xenos primitive type)
  // Non-indexed: 'count' vertices from start_vertex.
  // Indexed: 'count' indices from start_index; vertex = base_vertex + index.
  bool indexed = false;
  u32 start_vertex = 0;
  u32 count = 0;
  i32 base_vertex = 0;
  u32 start_index = 0;

  // DrawIndexedTessellatedVertices: 'primitive' is a patch type (triangle or
  // quad patches), base_vertex the first patch and 'count' the patch count;
  // in adaptive mode the indices from start_index on are the patches' edge
  // tessellation factors. Drawn as one instance per patch
  // (renutVertexRegisters in shader_common.h).
  bool tessellation = false;

  // Draw*UP / Begin*Vertices: stream 0 comes from here instead of its fetch
  // constant, and indices (when indexed) from up_indices: copies the renderer
  // owns, taken when the game's data was complete (queued.h).
  const u8 *up_vertices = nullptr;
  u32 up_stride = 0;
  u32 up_vertex_count = 0;
  const u8 *up_indices = nullptr;
  bool up_indices_32 = false;
};

// Reads the guest vertex buffers (and vertex fetch buffers) the draws recorded
// since the last call use into their host copies. Draws leave the reads for
// the device's KickOff, which calls this, as the GPU reads them only once the
// ring is kicked; the game may write them after the draw call until then.
// Also before the command list is submitted.
void FlushVertexFillsLocked();

// The float constants of a replayed draw: the groups of four registers its
// command buffer had set by the draw (m_Pending bit 63 - group) are in the
// draw's snapshot, the rest are the running device's.
struct ReplayConstants {
  const u8 *running = nullptr;
  uint64_t vs_set = 0, ps_set = 0;
  // Groups written since the last draw of this replay that uploaded its
  // constants (same bit order), which only those need comparing: Replay
  // gathers each op's writes, the upload consumes them. 'replay' tells
  // replays apart (0: unknown, compare everything); within one the running
  // device is the same.
  uint64_t *vs_dirty = nullptr, *ps_dirty = nullptr;
  uint64_t replay = 0;
};

// A draw from a device state snapshot (kDeviceSnapshotSize bytes laid out as
// the guest D3DDevice). Without 'replay', every float constant is the
// snapshot's.
bool DrawStateLocked(const u8 *device_state, const DrawCall &call,
                     const ReplayConstants *replay = nullptr);

// tw2de (2026-10-20, from sa2-nx, tw2de_renut_chain_reuse): set by backend/queued.cpp around a draw of its delta chain
// (tw2de_renut_snapshot_deltas): the device chunks that changed since the chain's previous draw. The chain's chunks: the
// 30 of 64 bytes before the float constants (device bytes 0-1919), then the bool constants on: tw2de (N&B's
// 12,712-byte snapshot) 41 (10112-12711, the last one 40 bytes); SA2 47 (10112-13087). 'seq' numbers the chain's draws (consecutive, from 2; a whole copy marks every chunk); 0 for
// any other draw. A memo of a draw's derived state is valid for the next draw when its seq is the memo's + 1 and none
// of the chunks it reads changed.
struct ChainHint {
  uint64_t seq = 0;
  uint64_t changed[2] = {};
};
extern ChainHint g_chain_hint;
// tw2de (2026-10-20): the next ChainHint::seq, shared by every producer of hints (backend/queued.cpp's delta chain,
// guest/command_buffers.cpp's replays): a producer whose number is not its previous one + 1 had another draw in between
// and marks every chunk changed.
uint64_t NextChainSeq();
constexpr u32 kChainHeadChunks = 30;
constexpr u32 kChainTailBegin = 10112;
// The chain chunk of a device byte outside the float constants.
constexpr u32 ChainChunkOf(u32 device_offset) {
  return device_offset < kChainTailBegin ? device_offset / 64 : kChainHeadChunks + (device_offset - kChainTailBegin) / 64;
}
// Whether any chunk of device bytes [first, last] changed (both outside the float constants, on the same side).
inline bool ChainChanged(const ChainHint &h, u32 first, u32 last) {
  for (u32 c = ChainChunkOf(first); c <= ChainChunkOf(last); ++c) {
    if ((h.changed[c / 64] >> (c % 64)) & 1)
      return true;
  }
  return false;
}

struct ClearRect {
  i32 x1, y1, x2, y2;
};

// Predicated tiling (D3DDevice_BeginTiling .. EndTiling): the tile rects, in
// screen space; count 0 ends tiling. A draw predicated to one tile
// (d3d::kDeviceOffsetPredication) is set up for that tile's EDRAM, which the
// GPU resolves to the tile's rect: the host cuts it to the rect, offsetting
// it there unless its viewport spans the screen (see ComputeViewport).
void SetTilesLocked(const ClearRect *rects, u32 count);

// The predication mask of the RunCommandBuffer being replayed, which the
// replayed draws' own masks narrow; ~0 outside a replay. Returns the previous.
u32 SetReplayPredicationLocked(u32 mask);

// D3DDevice_ClearF. flags: 0x1-0x8 render targets 0-3, 0x10 depth, 0x20 stencil.
void ClearStateLocked(const u8 *device_state, u32 flags, const ClearRect *rect,
                      const float color[4], float depth, u32 stencil);

// D3DDevice_Resolve. flags & 7: source (0-3 render target, 4 depth);
// 0x100 clear the color target after, 0x200 clear depth/stencil after.
// 'dest_format': the destination header's fetch constant when the resolve was
// issued, or nullptr to read the header now.
struct HostTexture;

// tw2de (2026-10-19, tw2de_renut_depth_words_lazy): 'dest' (a depth resolve destination) is read through its words:
// writes them now if its last resolve did not (from the unchanged source), and from now on at every resolve.
void EnsureDepthWordsLocked(HostTexture &dest);

void ResolveStateLocked(const u8 *device_state, u32 flags, const ClearRect *source_rect,
                        u32 dest_texture_va, const d3d::TextureFetchConstant *dest_format,
                        i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                        const float clear_color[4], float clear_depth, u32 clear_stencil);

// Reads render target 'source' (0-3) of the device state back for the CPU:
// what a color resolve of its rect (0, 0, width, height) into an X8R8G8B8
// texture would write, untiled, as width x height guest words (bytes
// FF RR GG BB each), row after row; the host texels a guest pixel covers at
// the target's render scale are averaged. 'width' and 'height' come back
// clamped to the target. Submits the open command list and waits for the GPU.
bool ReadRenderTargetLocked(const u8 *device_state, u32 source, u32 &width, u32 &height,
                            std::vector<u32> &words);

// What Swap scans out for the guest front buffer texture, or an empty image.
// tw2de: x1..y2 the Swap's scaler source rect (all 0: the whole texture); without them, the last Swap's.
PresentImage FrontBufferImageLocked(u32 texture_va, i32 x1, i32 y1, i32 x2, i32 y2);
PresentImage FrontBufferImageLocked(u32 texture_va);
// tw2de (2026-10-19): the texture at that address holds what a resolve wrote (not a new, uninitialised image).
bool FrontBufferWrittenLocked(u32 texture_va);

// A vertex declaration record at 'declaration_va' is being destroyed.
void ForgetVertexDeclarationLocked(u32 declaration_va);

// Once per guest Swap: ages the converted vertex stream cache and drops the
// streams no draw has read for a while.
void AdvanceStreamCacheFrameLocked();

// For the memory report: the converted vertex stream and index caches (the
// pool's host memory and what the copies in it take).
struct StreamCacheMemory {
  uint64_t pool_bytes = 0;
  uint64_t cached_bytes = 0;
  size_t streams = 0;
  size_t index_ranges = 0;
};
StreamCacheMemory StreamCacheMemoryLocked();

// tw2de (2026-10-27): textures made from memexport windows on the GPU (textures.cpp Upload): a tiled 2D level of
// whole blocks copied from the newest pending export window that holds it, untiled and byte-swapped by stream_convert_cs
// (its texture mode) into a buffer the level is copied from. A draw sampling such a texture does not wait for the
// exports (DrawNeedsExportFlush). deko3d only.
struct ExportUntile {
  u32 slot = 0;                // whose export buffer holds the window
  uint64_t source_offset = 0;  // of the level's storage (or its packed tail) there
  u32 pitch_blocks = 0;        // the tiling pitch
  u32 x_blocks = 0, y_blocks = 0;  // in a packed tail: where the level starts
  u32 blocks_w = 0, blocks_h = 0;
  u32 log2_block_bytes = 0;    // 2-4
  u32 endian = 0;
  u32 row_pitch = 0;           // of the destination rows (bytes)
  bool linear = false;         // tw2de (2026-10-30): a linear level (rows of pitch_blocks blocks), copied and swapped
};
// The pending export window holding all of [base, base + size) (physical), the newest: its slot and offset.
bool ExportWindowForRangeLocked(u32 base, u32 size, u32 &slot, uint64_t &offset);
// tw2de (2026-10-30, tw2de_memexport_zero_copy): the export windows are guest memory itself, through the GPU's
// mapping of all of physical memory: any level of a texture can be untiled from it on the GPU (UntileFromExportLocked
// with slot 0 and the physical address as the offset), pending exports or not. GuestRangeForGpuLocked first: the CPU's
// lines of the range cleaned to memory, so the GPU reads the game's writes.
bool ExportsZeroCopyLocked();
void GuestRangeForGpuLocked(u32 base, u32 size);
// And after the renderer itself wrote guest memory (the language pack's patches of the game's textures): cleaned whole.
void GuestRangeWrittenByHostLocked(u32 base, u32 size);
// tw2de (2026-10-30): the CPU's lines of the current slot's pending windows cleaned to memory, by the cards: right
// before the slot's lists are submitted (backend/video.cpp KickLocked; FinishRecordingLocked), since the kernels write
// guest memory itself once the GPU runs them. Nothing when the windows are not guest memory.
void CleanPendingExportsForGpuLocked();
// Whether [base, base + size) has pending exports at all.
// tw2de (2026-10-27, tw2de_diag_watch): the range overlaps the watched one.
bool WatchedRangeLocked(u32 base, u32 size);
// tw2de (2026-10-27): a fence wait's time (video.cpp), for the [burst] line.
void NoteGpuWaitMsLocked(double ms);
bool ExportsPendingForRangeLocked(u32 base, u32 size);
// Records the untile; 'out' is the level's rows (row_pitch apart) in a GPU buffer, ready to copy from.
bool UntileFromExportLocked(const ExportUntile &u, plume::RenderBufferReference &out);
// tw2de (2026-11-05, GPU untile): a tiled level from guest memory untiled on the GPU by the same kernel. Its storage
// ('guest', 'guest_bytes') is copied to the upload ring now (at record time, as the CPU untile reads it), then to the
// slot's stream buffer and untiled from there ('u': its layout; slot and source_offset are set here).
// tw2de (2026-11-05, the bursts): the render thread's work by kind ([rtwork], with tw2de_diag_vsync_timing): a
// Scope adds its wall time to a kind. Render thread only (g_log_ns: any thread).
namespace rtw {
enum Cat : uint32_t {
  kDraw = 0, kExportWait, kTargets, kGeometry, kExportStreams, kFetch, kTextures, kPipeline, kConstants, kClear,
  kResolve, kReplay, kReplayDraws, kShaderMake, kRelease, kKick, kMxPrepare, kMxCopyKernel, kMxComplete,
  kMxCompleteOutside, kGeoLookup, kGeoRead, kGeoTrack, kFrameItem, kPresent, kSubmitPublish, kWaitSlot, kLockWait,
  kHostShader, kDescribeFetch, kMiscItems, kKickVertexFills, kKickTextureFills, kDrawLookup, kPipeKey, kRecord, kDeclLookup, kUnused,
  kCount
};
extern double g_ms[kCount];
extern uint32_t g_n[kCount];
extern std::atomic<uint64_t> g_log_ns;  // deko3d.log's line writes (tw2de_renut_deko3d.cpp), any thread
extern uint64_t g_fill_stream_bytes, g_fill_copy_bytes;
// tw2de (2026-11-06, seen20): the lookups' counts: shader memo misses (FindShaderLocked), declarations built and
// cache clears (NativeDeclarationLocked); the stream fills by why no copy served them (TrackConvertedStream).
extern uint64_t g_shader_misses, g_decl_builds, g_decl_clears, g_decl_shared;
extern uint64_t g_fill_first_bytes, g_fill_changed_bytes, g_fill_waiting_bytes, g_fill_full_bytes, g_fill_untracked_bytes;  // tw2de (2026-11-05, seen18): the kicks' fills
extern bool g_in_replay;                // a command buffer replay is running (its draws: kReplayDraws too)
extern bool g_in_draw;                  // a draw is being recorded (kMxComplete then not also kMxCompleteOutside)
extern std::chrono::steady_clock::time_point g_frame_item_t0;  // the frame's present item began (device.cpp)
// TW2 (2026-11-13): inline. It was a call that called tw2de::frame_timing::On() (a function-static's guard), at every
// Scope, Span and DrawSegments: some thirty a draw, 1.2-1.8 % of the feed thread with the diagnostic off.
extern int g_on_state;  // -1: tw2de_diag_vsync_timing not read yet, 0 off, 1 on
bool OnInit();
inline bool On() { return g_on_state > 0 || (g_on_state < 0 && OnInit()); }
struct Scope {
  uint32_t cat;
  std::chrono::steady_clock::time_point t0;
  bool on;
  explicit Scope(uint32_t c) : cat(c), on(c != kUnused && On()) {
    if (on)
      t0 = std::chrono::steady_clock::now();
  }
  ~Scope() {
    if (on) {
      g_ms[cat] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      ++g_n[cat];
    }
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
};
// tw2de (2026-11-05, seen18): a Scope ended by hand (End) or at its end of life, whichever comes first.
struct Span {
  uint32_t cat;
  std::chrono::steady_clock::time_point t0;
  bool on;
  explicit Span(uint32_t c) : cat(c), on(c != kUnused && On()) {
    if (on)
      t0 = std::chrono::steady_clock::now();
  }
  void End() {
    if (on) {
      g_ms[cat] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      ++g_n[cat];
      on = false;
    }
  }
  ~Span() { End(); }
  Span(const Span &) = delete;
  Span &operator=(const Span &) = delete;
};
// TW2 (2026-11-12, diagnostic): DrawStateLocked's wall time between its markers (Mark(i) adds the time since the last
// mark to g_seg_ms[i]), for the [rtwork] line's draw segments.
constexpr uint32_t kSegs = 13;
extern double g_seg_ms[kSegs];
struct DrawSegments {
  bool on;
  std::chrono::steady_clock::time_point last;
  DrawSegments() : on(On()) {
    if (on)
      last = std::chrono::steady_clock::now();
  }
  void Mark(uint32_t i) {
    if (!on)
      return;
    const auto now = std::chrono::steady_clock::now();
    g_seg_ms[i] += std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
  }
};
template <class F>
auto Timed(uint32_t cat, F &&f) -> decltype(f()) {
  Scope scope(cat);
  return f();
}
}  // namespace rtw

// tw2de (2026-11-05): the render thread's work this frame by kind, as an [rtwork] line when it was busy 40 ms or
// more (tw2de_diag_vsync_timing). The render thread, once a frame (device.cpp FrameBookkeepingLocked).
void RenderWorkReportLocked(uint64_t frame, double busy_ms);
bool UntileFromGuestLocked(ExportUntile u, const uint8_t *guest, uint32_t guest_bytes, plume::RenderBufferReference &out);

} // namespace renut::gpu

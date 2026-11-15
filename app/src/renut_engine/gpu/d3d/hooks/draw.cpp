/**
 * @file    gpu/d3d/hooks/draw.cpp
 * @brief   Draw, clear and resolve entry points, and the shader setters.
 *
 * The draws replace N&B's D3D entirely: the originals would flush the device's
 * dirty state into GPU packets, including binding shaders by reading their
 * microcode, which renderer-owned shader objects do not carry. Clear, Resolve
 * and tiling queue their host work and do the device work of N&B's D3D
 * natively (d3d_native.h): dirty masks, the tiling state, fences on the
 * destination.
 *
 * The host work runs on the render thread (queued.h); a device recording a
 * command buffer records its draws, clears and resolves (command_buffers.h)
 * instead. Neither takes State().mutex here.
 */
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/cvar.h>
#include <string>
#include <fmt/format.h>
REXCVAR_DECLARE(bool, tw2de_diag_log);  // tw2de (2026-11-10): textures.cpp

#include "renut_engine/gpu/render/draw.h"  // tw2de (2026-11-05): rtw ([rtwork])
#include "renut_engine/gpu/tw2de_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/backend/video.h"

using namespace renut::gpu;

// tw2de (2026-10-26, diagnostic: the profiling build's black screen, 0 draws on the render thread): what the game's
// draw, clear and resolve calls did at the hooks, for the heartbeat (d3d/hooks/device.cpp, log_diagnostics):
// [0] draws seen, [1] of them skipped (not Active: host_ready false or shutting down), [2] recorded into a command
// buffer, [3] clears and resolves seen, [4] of them skipped, [5] recorded.
std::atomic<uint64_t> g_tw2de_hook_counts[6];
// tw2de (2026-11-12, tw2de_diag_frames: the black frames): this frame's resolve destinations (the first 8), for the
// Swap's per-frame line (device.cpp). Main thread only.
u32 g_tw2de_frame_resolve_dests[8];
u32 g_tw2de_frame_resolves = 0;

// A float argument also takes the GPR (or stack) slot of its position, which
// the marshaling does not skip: the u32 after each double in the hooks'
// parameters stands in for it.

namespace {

constexpr u32 kE_OUTOFMEMORY = 0x8007000Eu;
// D3DVIEWPORT9 {0, 0, 0xFFFF, 0xFFFF, 0.0, 1.0} in N&B's data (unk_820018DC):
// the whole surface.
constexpr u32 kFullViewport = 0x820018DC;
// D3DRS_HIZWRITEENABLE's index in the render state table.
constexpr u32 kRenderStateHiZWriteEnable = 97;

// Guest memory Begin*Vertices hands out; the game writes one draw's vertices
// into it before its next D3D call. One per device, as devices record on
// different threads.
constexpr u32 kVerticesScratchSize = 4u << 20;
std::mutex g_vertices_scratch_mutex;  // devices call from their own threads
std::unordered_map<u32, u32> g_vertices_scratch;

u32 VerticesScratch(u32 device) {
  std::lock_guard lock(g_vertices_scratch_mutex);
  u32 &scratch = g_vertices_scratch[device];
  if (!scratch)
    scratch = records::AllocPhysicalBlock(kVerticesScratchSize);
  return scratch;
}

bool Active() {
  auto &s = State();
  return s.host_ready && !s.shutting_down.load(std::memory_order_acquire);
}

// tw2de (2026-10-26): g_tw2de_hook_counts. kind 0: a draw, 3: a clear or a resolve.
void NoteHook(u32 device, int kind) {
  g_tw2de_hook_counts[kind].fetch_add(1, std::memory_order_relaxed);
  if (!Active())
    g_tw2de_hook_counts[kind + 1].fetch_add(1, std::memory_order_relaxed);
  else if (IsRecordingCommandBuffer(device))
    g_tw2de_hook_counts[kind + 2].fetch_add(1, std::memory_order_relaxed);
}

void StoreU32(u32 va, u32 value) {
  if (auto *p = GuestPtr<be_u32>(va))
    *p = value;
}

u32 LoadU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

bool ReadRect(u32 va, ClearRect &rect) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  rect = {i32(u32(p[0])), i32(u32(p[1])), i32(u32(p[2])), i32(u32(p[3]))};
  return true;
}

bool ReadVector4(u32 va, float out[4]) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  for (int i = 0; i < 4; ++i) {
    const u32 bits = p[i];
    std::memcpy(&out[i], &bits, 4);
  }
  return true;
}

// void D3DDevice_DrawVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartVertex,
//                             UINT VertexCount) [0x8222C7A0]
void D3DDevice_DrawVertices_hook(u32 device, u32 primitive, u32 start_vertex, u32 vertex_count) {
  NoteHook(device, 0);  // tw2de (2026-10-26, diagnostic)
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.start_vertex = start_vertex;
  call.count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex,
//                                    UINT StartIndex, UINT IndexCount) [0x826567C8]
void D3DDevice_DrawIndexedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                        u32 start_index, u32 index_count) {
  NoteHook(device, 0);  // tw2de (2026-10-26, diagnostic)
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedTessellatedVertices(D3DDevice*, D3DTESSPRIMITIVETYPE,
//     INT BaseVertexIndex, UINT StartIndex, UINT IndexCount) [0x82230838]
// Replaced like the other draws: the original's state flush binds the shader
// from its microcode, and bugchecks (0xF5) on a tessellation shader without
// the tessellation state the original SetVertexShader keeps. The draw is
// tessellated on the host (DrawCall::tessellation).
void D3DDevice_DrawIndexedTessellatedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                                   u32 start_index, u32 index_count) {
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.tessellation = true;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               const void *pVertexStreamZeroData, UINT Stride) [0x8222E520]
void D3DDevice_DrawVerticesUP_hook(u32 device, u32 primitive, u32 vertex_count, u32 data,
                                   u32 stride) {
  NoteHook(device, 0);  // tw2de (2026-10-26, diagnostic)
  if (!Active() || !data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(data);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_count * stride, 0);
  else
    QueueDraw(device, call, vertex_count * stride, 0);
}

// void D3DDevice_DrawIndexedVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT MinVertexIndex,
//     UINT NumVertices, UINT IndexCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
//     const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) [0x82656728]
void D3DDevice_DrawIndexedVerticesUP_hook(u32 device, u32 primitive, u32 min_vertex,
                                          u32 num_vertices, u32 index_count, u32 index_data,
                                          u32 index_format, u32 vertex_data, u32 stride) {
  NoteHook(device, 0);  // tw2de (2026-10-26, diagnostic)
  if (!Active() || !index_data || !vertex_data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.count = index_count;
  call.up_indices = GuestPtr<const u8>(index_data);
  call.up_indices_32 = (index_format & 4) != 0;  // D3DFMT_INDEX32
  call.up_vertices = GuestPtr<const u8>(vertex_data);
  call.up_stride = stride;
  call.up_vertex_count = min_vertex + num_vertices;
  const u32 vertex_bytes = call.up_vertex_count * stride;
  const u32 index_bytes = index_count * (call.up_indices_32 ? 4 : 2);
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_bytes, index_bytes);
  else
    QueueDraw(device, call, vertex_bytes, index_bytes);
}

// void *D3DDevice_BeginVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               UINT VertexStreamZeroStride) [0x8222CC50]
// Callers write the vertices, then end inline by restoring m_pRing from the
// device, so the draw's state is captured now and its vertices copied at the
// device's next hook (FlushDeferredVertices).
u32 D3DDevice_BeginVertices_hook(u32 device, u32 primitive, u32 vertex_count, u32 stride) {
  NoteHook(device, 0);  // tw2de (2026-10-26, diagnostic)
  const u64 bytes = u64(vertex_count) * stride;
  if (!bytes || bytes > kVerticesScratchSize)
    return 0;
  // The inline end: m_pRing = [device + kDeviceOffsetVerticesRingEnd].
  StoreU32(device + d3d::kDeviceOffsetVerticesRingEnd,
           LoadU32(device + u32(offsetof(d3d::Device, m_pRing))));

  const u32 scratch = VerticesScratch(device);
  if (!scratch)
    return 0;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(scratch);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  // Either way the device's previous Begin*Vertices data leaves the scratch
  // before the game writes this draw's over it.
  if (IsRecordingCommandBuffer(device))
    RecordDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else if (Active())
    QueueDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else
    FlushDeferredVertices(device);
  return scratch;
}

// A clear: the renderer's, recorded or queued, then N&B's device work.
// whole_target: BeginTiling's, of one tile's worth of EDRAM, which on the
// host is the whole tiled target.
void Clear(u32 device, u32 flags, const ClearRect *rect, const float color[4], float z,
           u32 stencil, bool whole_target) {
  NoteHook(device, 3);  // tw2de (2026-10-26, diagnostic)
  const ClearRect *host_rect = whole_target ? nullptr : rect;
  if (IsRecordingCommandBuffer(device))
    RecordClear(device, flags, host_rect, color, z, stencil);
  else if (Active())
    QueueClear(device, flags, host_rect, color, z, stencil);
  // tw2de: the device work is the game's own D3DDevice_ClearF, run by the hook.
}

// A resolve: the renderer's copy (and clear), then N&B's device work.
void Resolve(u32 device, u32 flags, const ClearRect *rect, u32 dest_texture, i32 dest_x,
             i32 dest_y, u32 dest_level, u32 dest_slice, u32 color_va, float clear_z,
             u32 clear_stencil, u32 parameters) {
  NoteHook(device, 3);  // tw2de (2026-10-26, diagnostic)
  if (g_tw2de_frame_resolves < 8)
    g_tw2de_frame_resolve_dests[g_tw2de_frame_resolves] = dest_texture;
  ++g_tw2de_frame_resolves;
  float color[4] = {0, 0, 0, 0};
  const bool has_color = color_va && ReadVector4(color_va, color);
  if (IsRecordingCommandBuffer(device)) {
    RecordResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                  has_color ? color : nullptr, clear_z, clear_stencil);
  } else if (Active()) {
    QueueResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                 has_color ? color : nullptr, clear_z, clear_stencil);
  }
  // tw2de: the device work is the game's own D3DDevice_Resolve, run by the hook.
  (void)parameters;
}

REX_EXTERN(__imp__sub_82D18CB8);  // tw2de: the game's D3DDevice_ClearF
// tw2de (2026-11-10): FF XIII-2's D3DDevice_Resolve is 8286A988 (XDK 20871): it calls an inner function,
// 82869830 (the fuzzy match to FF XIII's Resolve, hooked until today), with two new zero arguments before the
// destination (r6, r7), the rest moved two places on. Hooked there, the hook read the destination as 0 and every
// front buffer was left without a host texture (a black screen).
REX_EXTERN(__imp__sub_82D2C598);  // tw2de: the game's D3DDevice_Resolve
REX_EXTERN(__imp__sub_82D22DD8);  // tw2de: the game's D3DDevice_BeginTiling
REX_EXTERN(__imp__sub_82D23148);  // tw2de: the game's D3DDevice_EndTiling

// void D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT *pRect,
//                       const D3DVECTOR4 *pColor, float Z, DWORD Stencil) [0x822811A0]
// Z takes r7's slot, so Stencil arrives in r8.
void D3DDevice_ClearF_hook(u32 device, u32 flags, u32 rect_va, u32 color_va, double z,
                           u32 /*z_slot*/, u32 stencil) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  float color[4] = {0, 0, 0, 0};  // unk_82001160 without one
  if (color_va)
    ReadVector4(color_va, color);
  Clear(device, flags, has_rect ? &rect : nullptr, color, float(z), stencil, false);
  // tw2de: the game's ClearF for the device work (its packets go to the ring sink).
  rex::ppc::GuestToHostFunction<void>(__imp__sub_82D18CB8, device, flags, rect_va, color_va, z, 0u, stencil);
}

// void D3DDevice_Clear(D3DDevice*, DWORD Count, const D3DRECT *pRects, DWORD Flags,
//     D3DCOLOR Color, float Z, DWORD Stencil) [0x822812C8]: ClearF with the
// color as a vector (XMLoadColor: each byte as 1 + b / 2^23, scaled by
// 2^23 / 255 and offset), per rect. Z takes r8's slot.
void D3DDevice_Clear_hook(u32 device, u32 count, u32 rects_va, u32 flags, u32 color_argb,
                          double z, u32 /*z_slot*/, u32 stencil) {
  const float scale = std::bit_cast<float>(0x47008081u);  // aG, unk_82001150
  auto channel = [scale](u32 byte) {
    return std::fmaf(std::bit_cast<float>(0x3F800000u | (byte & 0xFF)), scale, -scale);
  };
  const float color[4] = {channel(color_argb >> 16), channel(color_argb >> 8),
                          channel(color_argb), channel(color_argb >> 24)};
  if (!rects_va) {
    Clear(device, flags, nullptr, color, float(z), stencil, false);
    return;
  }
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (ReadRect(rects_va + i * 16, rect))
      Clear(device, flags, &rect, color, float(z), stencil, false);
  }
}

// void D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT *pSourceRect,
//     D3DBaseTexture *pDestTexture, const D3DPOINT *pDestPoint, UINT DestLevel,
//     UINT DestSliceOrFace, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil, const D3DRESOLVE_PARAMETERS *pParameters) [0x8222D6C8]
// ClearZ is in f1 and takes the first stack slot; ClearStencil and pParameters
// are in the two after it.
void D3DDevice_Resolve_hook(u32 device, u32 flags, u32 rect_va, u32 dest_texture, u32 point_va,
                            u32 dest_level, u32 dest_slice, u32 color_va, double clear_z,
                            u32 /*z_slot*/, u32 clear_stencil, u32 parameters) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  // tw2de (2026-11-10, FF XIII-2's black screen: its front buffers have no host texture): the first resolves'
  // arguments and their destination's header words.
  if (REXCVAR_GET(tw2de_diag_log)) {
    static std::atomic<int> logged{0}, logged_tiles{0};
    // the first 24, and the first 24 of a tile's (a rect not starting at 0 or not the whole screen: the scene's tiles)
    const bool tile_rect = has_rect && (rect.x1 != 0 || rect.y1 != 0 || rect.x2 != 1280 || rect.y2 != 720);
    if (logged.fetch_add(1) < 24 || (tile_rect && logged_tiles.fetch_add(1) < 24)) {
      std::string words;
      if (const auto *h = GuestPtr<const be_u32>(dest_texture))
        for (int i = 0; i < 13; ++i)
          words += fmt::format(" {:08X}", u32(h[i]));
      i32 px = -1, py = -1;
      if (const auto *p = GuestPtr<const be_u32>(point_va)) {
        px = i32(u32(p[0]));
        py = i32(u32(p[1]));
      }
      RGPU_INFO("[resolve] flags {:08X} rect {} ({},{})-({},{}) point ({},{}) dest {:08X} level {} slice {} params "
                "{:08X};{}",
                flags, has_rect, has_rect ? rect.x1 : 0, has_rect ? rect.y1 : 0, has_rect ? rect.x2 : 0,
                has_rect ? rect.y2 : 0, px, py, dest_texture, dest_level, dest_slice, parameters, words);
    }
  }
  i32 dest_x = 0, dest_y = 0;
  if (const auto *p = GuestPtr<const be_u32>(point_va)) {
    dest_x = i32(u32(p[0]));
    dest_y = i32(u32(p[1]));
  }
  Resolve(device, flags, has_rect ? &rect : nullptr, dest_texture, dest_x, dest_y, dest_level,
          dest_slice, color_va, float(clear_z), clear_stencil, parameters);
  // tw2de: the game's Resolve for the device work.
  rex::ppc::GuestToHostFunction<void>(__imp__sub_82D2C598, device, flags, rect_va, dest_texture, point_va,
                                      dest_level, dest_slice, color_va, clear_z, 0u, clear_stencil, parameters);
}

// void D3DDevice_BeginTiling(D3DDevice*, DWORD Flags, DWORD Count,
//     const D3DRECT *pTileRects, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil) [0x822788A0]
// The surfaces bound from here to EndTiling are backed by targets covering
// every tile (SetTilingExtentLocked). N&B's BeginTiling clears with Z 0.0 and
// stencil 0 (its ClearZ and ClearStencil go unused), unless the device records
// a one-pass Z pass (Flags3 0x20); its ring segment for the tiles is left out.
void D3DDevice_BeginTiling_hook(u32 device, u32 flags, u32 count, u32 rects_va, u32 color_va,
                                double clear_z, u32 /*z_slot*/, u32 clear_stencil) {
  // tw2de: the surfaces bound from here to EndTiling are backed by targets covering every tile; the game's
  // BeginTiling then does the device work (its clears come through the hooked ClearF, its tile segment goes to the
  // ring sink and the draws are recorded once, on the full-size targets).
  std::vector<ClearRect> tiles;
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (!ReadRect(rects_va + i * 16, rect))
      break;
    tiles.push_back(rect);
  }
  if (REXCVAR_GET(tw2de_diag_log)) {  // tw2de (2026-11-10): FF XIII-2's tiles
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
      std::string r;
      for (const ClearRect &t : tiles)
        r += fmt::format(" ({},{})-({},{})", t.x1, t.y1, t.x2, t.y2);
      RGPU_INFO("[tiles] BeginTiling flags {:08X}, {} tiles:{}", flags, count, r);
    }
  }
  if (!IsRecordingCommandBuffer(device)) {
    FlushDeferredVertices(device);
    u32 width = 0, height = 0;
    for (const ClearRect &rect : tiles) {
      width = std::max(width, u32(rect.x2));
      height = std::max(height, u32(rect.y2));
    }
    render_thread::Queue([tiles, width, height] {
      const ::renut::gpu::rtw::Scope rtw_misc(::renut::gpu::rtw::kMiscItems);  // tw2de (2026-11-05, [rtwork])
      SetTilingExtentLocked(width, height);
      SetTilesLocked(tiles.data(), u32(tiles.size()));
    });
  }
  rex::ppc::GuestToHostFunction<void>(__imp__sub_82D22DD8, device, flags, count, rects_va, color_va, clear_z, 0u,
                                      clear_stencil);
}

// HRESULT D3DDevice_EndTiling(D3DDevice*, DWORD ResolveFlags, const D3DRECT *pResolveRects,
//     D3DBaseTexture *pDestTexture, const D3DVECTOR4 *pClearColor, float ClearZ, DWORD ClearStencil,
//     const D3DRESOLVE_PARAMETERS *pParameters)
// tw2de: the game's own EndTiling (its per-tile resolves come through the hooked Resolve, each reading its rect
// from the full-size target), then the host's tiling state is dropped.
u32 D3DDevice_EndTiling_hook(u32 device, u32 resolve_flags, u32 rects_va, u32 dest_texture, u32 color_va,
                             double clear_z, u32 /*z_slot*/, u32 clear_stencil, u32 parameters) {
  const u32 result = rex::ppc::GuestToHostFunction<u32>(__imp__sub_82D23148, device, resolve_flags, rects_va,
                                                         dest_texture, color_va, clear_z, 0u, clear_stencil,
                                                         parameters);
  if (!IsRecordingCommandBuffer(device)) {
    render_thread::Queue([] {
      const ::renut::gpu::rtw::Scope rtw_misc(::renut::gpu::rtw::kMiscItems);  // tw2de (2026-11-05, [rtwork])
      SetTilingExtentLocked(0, 0);
      SetTilesLocked(nullptr, 0);
    });
  }
  return result;
}

#if 0  // tw2de: N&B-only (export, Present and shader setters run as the game's own code)
// void D3DDevice_BeginExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x82269618]. The renderer finds a draw's memexport streams in its shaders.
void D3DDevice_BeginExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::BeginExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_EndExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x822693B0]: the resource is busy until the current fence, which completes
// once the render thread has written the stream's results.
void D3DDevice_EndExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::EndExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_Present(D3DDevice*) [0x82654778]: the automatic back buffer
// resolved into the automatic front buffer, then Swap. N&B creates its device
// without an automatic front buffer, so its callers (XUI, the threaded movie
// player) have nothing to resolve to.
void D3DDevice_Present_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return;
  FlushDeferredVertices(device);
  float viewport[6];
  for (u32 i = 0; i < 6; ++i)
    viewport[i] = d.F(0x3168 + i * 4);
  i32 scissor[4];
  for (u32 i = 0; i < 4; ++i)
    scissor[i] = i32(d.W(native::off::ScissorRect + i * 4));
  native::SetRenderTarget(d, 0, d.W(native::off::AutoBackBuffer));
  // SynchronizeToPresentationInterval (0x8228A748): its swap callback and wait
  // go into the ring; the swap is pending.
  native::SetPredication(d, 0);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | d3d::kSwapFlagPending);
  const u32 front_buffer = d.W(native::off::AutoFrontBuffer);
  if (front_buffer)
    Resolve(device, 0, nullptr, front_buffer, 0, 0, 0, 0, 0, 1.0f, 0, 0);
  native::KickOff(device);
  native::SetViewport(d, viewport[0], viewport[1], viewport[2], viewport[3], viewport[4],
                      viewport[5]);
  native::SetScissorRect(d, scissor[0], scissor[1], scissor[2], scissor[3]);
  native::Swap(device, front_buffer, device + 0x3608);
}

// void D3DDevice_SetVertexShader(D3DDevice*, D3DVertexShader*) [0x8222A0A8]
void D3DDevice_SetVertexShader_hook(u32 device, u32 shader) {
  native::SetVertexShader(native::DeviceAt(device), shader);
}

// void D3DDevice_SetPixelShader(D3DDevice*, D3DPixelShader*) [0x82226730]
void D3DDevice_SetPixelShader_hook(u32 device, u32 shader) {
  native::SetPixelShader(native::DeviceAt(device), shader);
}
#endif


} // namespace

TW2DE_RENUT_HOOK(sub_82D138B8, D3DDevice_DrawVertices_hook)  /* N&B sub_8222C7A0 */;  // D3DDevice_DrawVertices
TW2DE_RENUT_HOOK(sub_82D13CD0, D3DDevice_DrawIndexedVertices_hook)  /* N&B sub_826567C8 */;  // D3DDevice_DrawIndexedVertices
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK_SHARED(sub_82230838, D3DDevice_DrawIndexedTessellatedVertices_hook);  // D3DDevice_DrawIndexedTessellatedVertices
TW2DE_RENUT_HOOK(sub_82D13290, D3DDevice_DrawVerticesUP_hook)  /* N&B sub_8222E520 */;  // D3DDevice_DrawVerticesUP
TW2DE_RENUT_HOOK(sub_82D13818, D3DDevice_DrawIndexedVerticesUP_hook)  /* N&B sub_82656728 */;  // D3DDevice_DrawIndexedVerticesUP
TW2DE_RENUT_HOOK(sub_82D12DD0, D3DDevice_BeginVertices_hook)  /* N&B sub_8222CC50 */;  // D3DDevice_BeginVertices
TW2DE_RENUT_HOOK(sub_82D18CB8, D3DDevice_ClearF_hook)  /* N&B sub_822811A0 */;  // D3DDevice_ClearF
// tw2de: not hooked (the game's own code runs): D3DDevice_Clear_hook
TW2DE_RENUT_HOOK(sub_82D2C598, D3DDevice_Resolve_hook)  /* N&B sub_8222D6C8 */;  // D3DDevice_Resolve
TW2DE_RENUT_HOOK(sub_82D22DD8, D3DDevice_BeginTiling_hook);  // D3DDevice_BeginTiling (tw2de: manual match)
TW2DE_RENUT_HOOK(sub_82D23148, D3DDevice_EndTiling_hook)  /* N&B sub_82212328 */;  // D3DDevice_EndTiling
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK_SHARED(sub_82269618, D3DDevice_BeginExport_hook);  // D3DDevice_BeginExport
// tw2de: not hooked (the game's own code runs): D3DDevice_EndExport_hook
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK_SHARED(sub_82654778, D3DDevice_Present_hook);  // D3DDevice_Present
// tw2de: not hooked (the game's own code runs): D3DDevice_SetVertexShader_hook
// tw2de: not hooked (the game's own code runs): D3DDevice_SetPixelShader_hook

/**
 * @file    gpu/backend/guest_gpu_memory.cpp
 * @brief   tw2de (2026-10-30): the GPU's mapping of guest physical memory. See guest_gpu_memory.h.
 */
#include "renut_engine/gpu/backend/guest_gpu_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"  // IsDeko3D
#include "renut_engine/gpu/backend/memory_report.h"  // HeapRoomBytes
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/backend/video_internal.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"  // StoreCardsOn
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>
#endif

REXCVAR_DEFINE_BOOL(tw2de_memexport_zero_copy, true, "Game/Renut",
                    "deko3d: the memexport kernels write guest memory itself through a GPU mapping of all of physical "
                    "memory, made once at the first frame (every chunk committed, every view mapped): no seed copies "
                    "into an export buffer, no copies of the results back, no export buffers, and the textures the "
                    "kernels make are untiled from guest memory on the GPU. Off, or when the mapping cannot be made: "
                    "the export buffers, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_memexport_zero_copy_gpu_cached, true, "Game/Renut",
                    "With tw2de_memexport_zero_copy: the GPU's mapping of guest memory goes through its L2 (the "
                    "kernels' dword stores coalesce there; the L2 is invalidated before a draw reads guest memory the "
                    "CPU may have written). Off: uncached, every GPU access goes to memory (slower kernels, no "
                    "invalidations)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__) && defined(PLUME_DEKO3D)
// sdk/src/core/guest_memory_switch.h (the SDK's include directory is not this target's). RexGmCommit's access is the
// RexGmAccess enum there (REX_GM_WRITE = 3), an int in the ABI.
extern "C" bool RexGmCommit(size_t offset, size_t length, int access);
extern "C" bool RexGmPremapViews(size_t offset, size_t length, size_t* mapped_out);
extern "C" uint8_t* RexGmContiguousShadow(size_t offset, size_t length);
extern "C" bool RexGmPin(size_t offset, size_t length, bool pin);
extern "C" bool RexGmWindowToOffset(uint64_t window_address, size_t* offset_out);
extern "C" size_t RexGmCommittedBytes(void);
extern "C" size_t RexGmMappedBytes(void);
extern "C" uint32_t RexGmLastResult(void);

namespace plume {
void deko3dLog(const char *fmt, ...);  // plume_deko3d.cpp (deko3d.log)
}
#endif

namespace renut::gpu::guest_gpu {

namespace {

constexpr uint32_t kPhysicalBytes = 0x20000000u;  // 512 MB of guest physical memory (sdk xmemory.cpp map_info)
constexpr uint32_t kPages = kPhysicalBytes >> 12;

struct Mapping {
  bool tried = false, on = false, gpu_cached = true;
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint8_t *memory = nullptr;  // the raw physical view (GuestPhysicalPtr(0)): cache maintenance by these addresses
  bool l2_invalidated_this_draw = false;
  Stats stats;
  // CleanForGpu: the interval (GuestWriteInterval) each physical page was last cleaned in (0: never), and the count of
  // writes the cards had found on it then (GuestPageWriteCount).
  uint32_t clean_serial[kPages] = {};
  uint32_t clean_writes[kPages] = {};
} g_map;

double MsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

#if defined(__SWITCH__) && defined(PLUME_DEKO3D)
// Both logs: the game's, and deko3d.log (written at once, so a crash's last line is there).
template <typename... Args>
void Say(bool warn, fmt::format_string<Args...> f, Args &&...args) {
  const std::string line = fmt::format(f, std::forward<Args>(args)...);
  if (warn)
    RGPU_WARN("[guestgpu] {}", line);
  else
    RGPU_INFO("[guestgpu] {}", line);
  plume::deko3dLog("[guestgpu] %s", line.c_str());
}

// The mapping, once: false (with the reason logged) leaves the old path on.
bool Enable(VideoState &s) {
  if (!s.device || !IsDeko3D()) {
    Say(true, "not on deko3d: the export buffers stay");
    return false;
  }
  uint8_t *memory = GuestPhysicalPtr<uint8_t>(0);
  size_t offset = 0;
  if (!memory || !RexGmWindowToOffset(reinterpret_cast<uint64_t>(memory), &offset)) {
    Say(true, "the physical view is not in the guest window: the export buffers stay");
    return false;
  }
  const bool gpu_cached = REXCVAR_GET(tw2de_memexport_zero_copy_gpu_cached);
  const auto t0 = std::chrono::steady_clock::now();
  const size_t committed_before = RexGmCommittedBytes(), mapped_before = RexGmMappedBytes();
  const unsigned long long room_before = HeapRoomBytes();
  // 1. Every chunk committed, 4 MB at a time (the exception handler's granule; one call for the 512 MB would ask the
  // heap for a gap's whole size in one block).
  constexpr size_t kStep = 4u << 20;
  for (size_t at = 0; at < kPhysicalBytes; at += kStep) {
    if (!RexGmCommit(offset + at, kStep, 3 /* REX_GM_WRITE */)) {
      Say(true, "committing physical memory at {:#x} failed ({:#x}, heap room {} MB): the export buffers stay", at,
          RexGmLastResult(), HeapRoomBytes() >> 20);
      return false;
    }
  }
  const double commit_ms = MsSince(t0);
  // 2. Every view of it mapped (the kernel refuses a mapping once the memory is device-mapped).
  const auto t1 = std::chrono::steady_clock::now();
  size_t premapped = 0;
  if (!RexGmPremapViews(offset, kPhysicalBytes, &premapped)) {
    Say(true, "mapping every view of physical memory failed after {} MB ({:#x}; {} MB mapped in all): the export "
              "buffers stay",
        premapped >> 20, RexGmLastResult(), RexGmMappedBytes() >> 20);
    return false;
  }
  const double premap_ms = MsSince(t1);
  // 3. The shadow of all of it, one range.
  uint8_t *shadow = RexGmContiguousShadow(offset, kPhysicalBytes);
  if (!shadow) {
    Say(true, "the shadows of physical memory are not one range (the shadows' reservation was not made at start): "
              "the export buffers stay");
    return false;
  }
  // 4. The GPU's block over it.
  const auto t2 = std::chrono::steady_clock::now();
  std::unique_ptr<plume::RenderBuffer> buffer =
      plume::Deko3DCreateBufferOverMemory(s.device.get(), shadow, kPhysicalBytes, gpu_cached, "guest memory");
  if (!buffer) {
    Say(true, "deko3d could not map the {} MB shadow for the GPU (deko3d.log has its message): the export buffers stay",
        kPhysicalBytes >> 20);
    return false;
  }
  const double map_ms = MsSince(t2);
  // 5. Pinned: protection changes recorded, no decommit, accesses through a view that could not be mapped emulated.
  RexGmPin(offset, kPhysicalBytes, true);
  g_map.buffer = std::move(buffer);
  g_map.memory = memory;
  g_map.gpu_cached = gpu_cached;
  g_map.on = true;
  Say(false, "guest physical memory mapped for the GPU (GPU {}): {} MB committed now ({} MB were), every view "
             "mapped (+{} MB, {} MB mapped in all), block made in {:.1f} ms (commit {:.1f} ms, views {:.1f} ms); heap "
             "room {} MB (was {} MB); GPU address {:#x}. Memexport writes guest memory itself from here on",
      gpu_cached ? "cached" : "uncached", (RexGmCommittedBytes() - committed_before) >> 20, committed_before >> 20,
      premapped >> 20, (mapped_before + premapped) >> 20, map_ms, commit_ms, premap_ms, HeapRoomBytes() >> 20,
      room_before >> 20, g_map.buffer->getDeviceAddress());
  return true;
}
#endif

}  // namespace

void FrameLocked(VideoState &s) {
#if defined(__SWITCH__) && defined(PLUME_DEKO3D)
  if (g_map.tried)
    return;
  g_map.tried = true;
  if (!REXCVAR_GET(tw2de_memexport_zero_copy)) {
    RGPU_INFO("[guestgpu] tw2de_memexport_zero_copy is off: the export buffers, as before");
    return;
  }
  Enable(s);
#else
  (void)s;
  g_map.tried = true;
#endif
}

bool On() {
  return g_map.on;
}

plume::RenderBuffer *Buffer() {
  return g_map.on ? g_map.buffer.get() : nullptr;
}

void CleanForGpu(uint32_t physical, uint32_t size) {
#if defined(__SWITCH__)
  if (!g_map.on || !size || physical >= kPhysicalBytes)
    return;
  size = std::min<uint32_t>(size, kPhysicalBytes - physical);
  const auto t0 = std::chrono::steady_clock::now();
  const bool cards = tw2de::renut_gpu::StoreCardsOn();
  const uint32_t now = GuestWriteInterval();
  const uint32_t first = physical >> 12, last = (physical + size - 1) >> 12;
  const uint32_t end = physical + size;
  // The cards of the range taken first, eight pages a load (HarvestGuestPagesLocked), then the counts compared: a page
  // needs the clean when it was never cleaned, when the cards have found a write by the game or a host writer on it
  // since its last clean (the count of them changed), or, without the cards, once an interval. An unaligned store marks
  // its first byte's page only and may run into the next page's first line: a run's clean takes that line too.
  // (v2 asked GuestPageWriteCount for each page: four atomic loads and a call a page, 0.5 ms a 32 MB window.)
  const uint32_t *counts = nullptr;
  if (cards) {
    HarvestGuestPagesLocked(first, last);
    counts = GuestPageWriteCounts();
  }
  auto needs = [&](uint32_t p) {
    const uint32_t serial = g_map.clean_serial[p];
    if (serial == 0)
      return true;
    if (!cards)
      return serial != now;
    return counts[p] != g_map.clean_writes[p];
  };
  for (uint32_t p = first; p <= last;) {
    if (!needs(p)) {
      ++g_map.stats.clean_skipped_pages;
      ++p;
      continue;
    }
    uint32_t q = p;
    while (q <= last && needs(q)) {
      g_map.clean_serial[q] = now;
      g_map.clean_writes[q] = cards ? counts[q] : 0;
      ++q;
    }
    const uint32_t lo = std::max(physical, p << 12), hi = std::min(end, (q << 12) + 64u);
    armDCacheClean(g_map.memory + lo, hi - lo);
    g_map.stats.clean_bytes += hi - lo;
    p = q;
  }
  g_map.stats.clean_ms += MsSince(t0);
#else
  (void)physical;
  (void)size;
#endif
}

void CleanForGpuNow(uint32_t physical, uint32_t size) {
#if defined(__SWITCH__)
  if (!g_map.on || !size || physical >= kPhysicalBytes)
    return;
  size = std::min<uint32_t>(size, kPhysicalBytes - physical);
  const auto t0 = std::chrono::steady_clock::now();
  armDCacheClean(g_map.memory + physical, size);
  g_map.stats.clean_bytes += size;
  g_map.stats.clean_ms += MsSince(t0);
#else
  (void)physical;
  (void)size;
#endif
}

void InvalidateAfterGpu(uint32_t physical, uint32_t size) {
#if defined(__SWITCH__)
  if (!g_map.on || !size || physical >= kPhysicalBytes)
    return;
  size = std::min<uint32_t>(size, kPhysicalBytes - physical);
  const auto t0 = std::chrono::steady_clock::now();
  armDCacheFlush(g_map.memory + physical, size);  // dc civac
  g_map.stats.invalidate_bytes += size;
  g_map.stats.invalidate_ms += MsSince(t0);
#else
  (void)physical;
  (void)size;
#endif
}

bool InvalidateGpuCachesLocked(VideoState &s) {
#if defined(PLUME_DEKO3D)
  if (!g_map.on || !g_map.gpu_cached || g_map.l2_invalidated_this_draw || !s.command_list)
    return false;
  plume::Deko3DFlushToMemory(s.command_list);
  g_map.l2_invalidated_this_draw = true;
  return true;
#else
  (void)s;
  return false;
#endif
}

void NewDrawLocked() {
  g_map.l2_invalidated_this_draw = false;
}

Stats TakeStats() {
  const Stats out = g_map.stats;
  g_map.stats = {};
  return out;
}

}  // namespace renut::gpu::guest_gpu

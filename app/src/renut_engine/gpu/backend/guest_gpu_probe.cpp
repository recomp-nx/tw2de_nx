/**
 * @file    gpu/backend/guest_gpu_probe.cpp
 * @brief   tw2de (2026-10-30): the GPU over the guest's memory, tried once (guest_gpu_probe.h).
 */
#include "renut_engine/gpu/backend/guest_gpu_probe.h"

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
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/backend/video_internal.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"

#if defined(__SWITCH__) && defined(PLUME_DEKO3D)
#include <malloc.h>
#include <switch.h>

#include <plume_deko3d.h>
#endif

REXCVAR_DEFINE_INT32(tw2de_diag_gpu_guest_probe, 0, "Game/Diagnostics",
                     "tw2de diagnostic (2026-10-30): can the GPU map the guest's memory (towards memexport "
                     "kernels that write it directly, no seed and no results copy)? 1: a 2 MB region made like a "
                     "guest memory chunk (code alias), GPU-mapped, copied out of and into by the GPU, its view "
                     "mapped and protected meanwhile; 2: also a committed chunk of the real guest memory, GPU-mapped "
                     "and read. [gpuprobe] lines in the game log and deko3d.log. 0: off")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_diag_gpu_guest_probe_frame, 240, "Game/Diagnostics",
                     "tw2de diagnostic (2026-10-30): the render thread frame tw2de_diag_gpu_guest_probe runs at")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_diag_gpu_guest_probe_cached, false, "Game/Diagnostics",
                    "tw2de diagnostic (2026-10-30): the probe's GPU mapping with the GPU's cache on (false: "
                    "uncached, what kernels writing guest memory would use)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__) && defined(PLUME_DEKO3D)
// sdk/src/core/guest_memory_switch.h (the SDK's include directory is not this target's).
extern "C" bool RexGmShadowFrom(size_t from, size_t* offset_out, uint8_t** shadow_out, size_t* length_out);
extern "C" bool RexGmMapAllViews(size_t offset);

namespace plume {
void deko3dLog(const char *fmt, ...);  // plume_deko3d.cpp (deko3d.log)
}
#endif

namespace renut::gpu {

#if defined(__SWITCH__) && defined(PLUME_DEKO3D)
namespace {

constexpr size_t kProbeHalf = 1u << 20;      // two of these, mapped next to each other
constexpr size_t kProbeBytes = 2 * kProbeHalf;
constexpr size_t kPage = 0x1000;
// Guest physical memory starts at mapping offset 4 GB in the guest window (sdk xmemory.cpp map_info).
constexpr size_t kPhysicalMappingOffset = 0x100000000ull;

struct Probe {
  int phase = 0;  // 0 not run, 1 run (buffers retired), 2 cleaned up
  uint64_t frames = 0, done_at = 0;
  void *backing[2] = {};
  uint8_t *shadow = nullptr;
  VirtmemReservation *shadow_reservation = nullptr;
  // Two views of the region: A mapped before the GPU mapping and kept through it (a view the game has already, as
  // every view would be once mapped up front); B mapped, protected and unmapped while the GPU mapping exists (what
  // RexGmFaultIn and RexGmProtect would do to a view the game had not touched yet).
  uint8_t *view[2] = {};
  VirtmemReservation *view_reservation[2] = {};
  bool view_mapped[2] = {};
} g_probe;

// Both logs: the game's, and deko3d.log (written at once, so a crash's last line is there).
template <typename... Args>
void Say(bool warn, fmt::format_string<Args...> f, Args &&...args) {
  const std::string line = fmt::format(f, std::forward<Args>(args)...);
  if (warn)
    RGPU_WARN("[gpuprobe] {}", line);
  else
    RGPU_INFO("[gpuprobe] {}", line);
  plume::deko3dLog("[gpuprobe] %s", line.c_str());
}

Handle Proc() {
  const Handle h = envGetOwnProcessHandle();
  return h != INVALID_HANDLE ? h : CUR_PROCESS_HANDLE;
}

// svcQueryMemory of an address: "type T perm P attr A" (libnx MemoryType / MemoryAttribute values; attr bit 2 (4) is
// IsDeviceMapped).
std::string MemoryStateOf(const void *address) {
  MemoryInfo info{};
  u32 page_info = 0;
  const Result rc = svcQueryMemory(&info, &page_info, reinterpret_cast<u64>(address));
  if (R_FAILED(rc))
    return fmt::format("svcQueryMemory failed {:#x}", rc);
  return fmt::format("type {} perm {} attr {}{} [{:#x}+{:#x})", u32(info.type), u32(info.perm), u32(info.attr),
                     (info.attr & 4u) ? " (device mapped)" : "", info.addr, info.size);
}

u32 CountDifferentDwords(const void *a, const void *b, size_t bytes, size_t *first_out) {
  const uint32_t *x = static_cast<const uint32_t *>(a), *y = static_cast<const uint32_t *>(b);
  u32 differ = 0;
  size_t first = SIZE_MAX;
  for (size_t i = 0; i < bytes / 4; ++i) {
    if (x[i] != y[i]) {
      if (!differ)
        first = i * 4;
      ++differ;
    }
  }
  if (first_out)
    *first_out = first;
  return differ;
}

void FillPattern(void *memory, size_t bytes, uint32_t salt) {
  uint32_t *w = static_cast<uint32_t *>(memory);
  for (size_t i = 0; i < bytes / 4; ++i)
    w[i] = uint32_t(i) * 2654435761u ^ salt;
}

// A GPU copy, run to completion now (the open list is submitted and waited; a new one opens).
bool GpuCopyNow(VideoState &s, plume::RenderBuffer *dst, uint64_t dst_offset, plume::RenderBuffer *src,
                uint64_t src_offset, uint64_t bytes) {
  if (!Video::EnsureCommandListLocked() || !s.command_list)
    return false;
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderBufferBarrier before[2] = {plume::RenderBufferBarrier(src, plume::RenderBufferAccess::READ),
                                         plume::RenderBufferBarrier(dst, plume::RenderBufferAccess::WRITE)};
  cmd->barriers(plume::RenderBarrierStage::COPY, before, 2);
  cmd->copyBufferRegion(plume::RenderBufferReference(dst, dst_offset), plume::RenderBufferReference(src, src_offset),
                        bytes);
  plume::RenderBufferBarrier after[2] = {plume::RenderBufferBarrier(src, plume::RenderBufferAccess::WRITE),
                                        plume::RenderBufferBarrier(dst, plume::RenderBufferAccess::WRITE)};
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, after, 2);
  const auto start = std::chrono::steady_clock::now();
  detail::SubmitAndWaitLocked(s);
  Say(false, "  GPU copy of {} KiB submitted and waited in {:.2f} ms", bytes >> 10,
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return true;
}

// The region made as a guest memory chunk is: heap memory mapped as a code alias at a chosen address, then RW.
bool MakeShadowRegion() {
  Probe &p = g_probe;
  for (int i = 0; i < 2; ++i) {
    p.backing[i] = memalign(kProbeHalf, kProbeHalf);
    if (!p.backing[i]) {
      Say(true, "no heap for the {} MB backing", kProbeHalf >> 20);
      return false;
    }
    std::memset(p.backing[i], 0, kProbeHalf);
  }
  virtmemLock();
  p.shadow = static_cast<uint8_t *>(virtmemFindCodeMemory(kProbeBytes, kProbeHalf));
  if (p.shadow)
    p.shadow_reservation = virtmemAddReservation(p.shadow, kProbeBytes);
  Result rc[2] = {};
  if (p.shadow) {
    for (int i = 0; i < 2; ++i)
      rc[i] = svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(p.shadow) + i * kProbeHalf,
                                      reinterpret_cast<u64>(p.backing[i]), kProbeHalf);
  }
  virtmemUnlock();
  if (!p.shadow) {
    Say(true, "no code address space for the {} MB region", kProbeBytes >> 20);
    return false;
  }
  Say(false, "region at {:#x}: svcMapProcessCodeMemory of its two 1 MB halves: {:#x}, {:#x}", reinterpret_cast<u64>(p.shadow),
      rc[0], rc[1]);
  if (R_FAILED(rc[0]) || R_FAILED(rc[1]))
    return false;
  for (int i = 0; i < 2; ++i) {
    const Result perm = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(p.shadow) + i * kProbeHalf, kProbeHalf,
                                                      Perm_Rw);
    if (R_FAILED(perm)) {
      Say(true, "svcSetProcessMemoryPermission(RW) of half {}: {:#x}", i, perm);
      return false;
    }
  }
  Say(false, "region's state: first half {}; second half {}", MemoryStateOf(p.shadow), MemoryStateOf(p.shadow + kProbeHalf));
  return true;
}

// A view of the region (A or B), reserved address space for it once.
bool FindView(int k) {
  Probe &p = g_probe;
  if (p.view[k])
    return true;
  virtmemLock();
  p.view[k] = static_cast<uint8_t *>(virtmemFindAslr(kProbeBytes, kProbeHalf));
  if (p.view[k])
    p.view_reservation[k] = virtmemAddReservation(p.view[k], kProbeBytes);
  virtmemUnlock();
  if (!p.view[k])
    Say(true, "no address space for view {}", k == 0 ? 'A' : 'B');
  return p.view[k] != nullptr;
}

bool MapView(int k, const char *when) {
  Probe &p = g_probe;
  if (!FindView(k))
    return false;
  const Result rc = svcMapProcessMemory(p.view[k], Proc(), reinterpret_cast<u64>(p.shadow), kProbeBytes);
  Say(R_FAILED(rc), "svcMapProcessMemory of the region into view {} {}: {:#x}{}", k == 0 ? 'A' : 'B', when, rc,
      R_FAILED(rc) && k == 1 ? " (RexGmFaultIn would fail: every view must be mapped before the GPU mapping)" : "");
  p.view_mapped[k] = R_SUCCEEDED(rc);
  return p.view_mapped[k];
}

void UnmapView(int k, const char *when) {
  Probe &p = g_probe;
  if (!p.view_mapped[k])
    return;
  const Result rc = svcUnmapProcessMemory(p.view[k], Proc(), reinterpret_cast<u64>(p.shadow), kProbeBytes);
  Say(R_FAILED(rc), "svcUnmapProcessMemory of view {} {}: {:#x}{}", k == 0 ? 'A' : 'B', when, rc,
      R_FAILED(rc) ? " (RexGmProtect's unmapping watch would fail)" : "");
  if (R_SUCCEEDED(rc))
    p.view_mapped[k] = false;
}

// What RexGmFaultIn and RexGmProtect do to a view: mapped, read, its first page's permission lowered and restored,
// unmapped.
void ViewBTests(const char *when) {
  Probe &p = g_probe;
  if (!MapView(1, when))
    return;
  size_t first = 0;
  const u32 differ = CountDifferentDwords(p.view[1], p.shadow, kProbeBytes, &first);
  Say(differ != 0, "  view B reads {} dwords differently from the shadow (first at {:#x})", differ, first);
  Say(false, "  view B's state: {}", MemoryStateOf(p.view[1]));
  Result rc = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(p.view[1]), kPage, Perm_R);
  Say(false, "  svcSetProcessMemoryPermission(R) of view B's first page {}: {:#x} (RexGmProtect's read-only watch)", when,
      rc);
  if (R_SUCCEEDED(rc)) {
    rc = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(p.view[1]), kPage, Perm_Rw);
    Say(R_FAILED(rc), "  and back to RW: {:#x}", rc);
  }
  UnmapView(1, when);
}

void RunRegionProbe(VideoState &s, bool cached) {
  Probe &p = g_probe;
  if (!MakeShadowRegion())
    return;
  ViewBTests("without a GPU mapping");
  MapView(0, "before the GPU mapping (kept through it)");
  // The CPU's pattern, in memory for the GPU.
  FillPattern(p.shadow, kProbeBytes, 0xA5A5A5A5u);
  armDCacheClean(p.shadow, kProbeBytes);
  plume::deko3dLog("[gpuprobe] making the GPU's block over the code alias now (a crash here is the kernel or deko3d "
                   "refusing it)");
  std::unique_ptr<plume::RenderBuffer> region =
      plume::Deko3DCreateBufferOverMemory(s.device.get(), p.shadow, kProbeBytes, cached, "guest probe region");
  if (!region) {
    Say(true, "RESULT: no GPU mapping of a code alias (deko3d.log has deko3d's words). The GPU cannot address guest "
              "memory this way");
    return;
  }
  Say(false, "RESULT: the code alias is GPU-mapped (GPU {}). State now: {}", cached ? "cached" : "uncached",
      MemoryStateOf(p.shadow));
  Say(false, "  second half's state: {}", MemoryStateOf(p.shadow + kProbeHalf));

  // GPU reads: the whole region copied out, both halves (two kernel memory blocks under one GPU mapping).
  std::unique_ptr<plume::RenderBuffer> readback = s.device->createBuffer(
      plume::RenderBufferDesc::ReadbackBuffer(kProbeBytes, plume::RenderBufferFlag::HOST_CACHED));
  std::unique_ptr<plume::RenderBuffer> upload = s.device->createBuffer(
      plume::RenderBufferDesc::UploadBuffer(kProbeHalf, plume::RenderBufferFlag::HOST_CACHED));
  if (!readback || !upload) {
    Say(true, "no readback or upload buffer for the tests");
  } else {
    if (GpuCopyNow(s, readback.get(), 0, region.get(), 0, kProbeBytes)) {
      const plume::RenderRange all(0, kProbeBytes);
      const uint8_t *got = static_cast<const uint8_t *>(readback->map(0, &all));
      size_t first = 0;
      const u32 differ_lo = CountDifferentDwords(got, p.shadow, kProbeHalf, &first);
      size_t first_hi = 0;
      const u32 differ_hi = CountDifferentDwords(got + kProbeHalf, p.shadow + kProbeHalf, kProbeHalf, &first_hi);
      Say(differ_lo || differ_hi,
          "RESULT GPU read: {} dwords wrong in the first half (first at {:#x}), {} in the second (first at {:#x}){}",
          differ_lo, first, differ_hi, first_hi, (differ_lo || differ_hi) ? "" : ": the GPU reads what the CPU wrote");
      readback->unmap(0, nullptr);
    }
    // GPU writes: a pattern copied into the middle MB (across the two halves); the rest must keep the CPU's pattern.
    const plume::RenderRange up(0, kProbeHalf);
    uint8_t *src = static_cast<uint8_t *>(upload->map(0, &up));
    FillPattern(src, kProbeHalf, 0x5A5A5A5Au);
    upload->unmap(0, &up);
    const uint64_t at = kProbeHalf / 2;
    if (GpuCopyNow(s, region.get(), at, upload.get(), 0, kProbeHalf)) {
      // The CPU's lines of the region dropped (dc civac) before it reads what the GPU wrote.
      const plume::RenderRange whole(0, kProbeBytes);
      region->map(0, &whole);
      size_t first = 0;
      const u32 differ_written = CountDifferentDwords(p.shadow + at, src, kProbeHalf, &first);
      // Around it: the CPU's pattern as filled.
      std::unique_ptr<uint32_t[]> expect(new uint32_t[kProbeBytes / 4]);
      FillPattern(expect.get(), kProbeBytes, 0xA5A5A5A5u);
      size_t first_before = 0, first_after = 0;
      const u32 differ_before = CountDifferentDwords(p.shadow, expect.get(), at, &first_before);
      const u32 differ_after = CountDifferentDwords(p.shadow + at + kProbeHalf, reinterpret_cast<const uint8_t *>(expect.get()) + at + kProbeHalf,
                                                    kProbeBytes - at - kProbeHalf, &first_after);
      Say(differ_written || differ_before || differ_after,
          "RESULT GPU write: {} dwords wrong in the MB the GPU wrote (first at {:#x}); {} changed before it, {} after it{}",
          differ_written, first, differ_before, differ_after,
          (differ_written || differ_before || differ_after) ? "" : ": the CPU reads what the GPU wrote");
      // And through view A (the same physical memory; the lines were dropped above).
      if (p.view_mapped[0]) {
        size_t first_view = 0;
        const u32 differ_view = CountDifferentDwords(p.view[0] + at, src, kProbeHalf, &first_view);
        Say(differ_view != 0, "RESULT GPU write seen through view A: {} dwords wrong (first at {:#x}){}", differ_view,
            first_view, differ_view ? "" : ": a view the game has sees the GPU's writes");
      }
    }
  }
  ViewBTests("with the GPU mapping");
  UnmapView(0, "with the GPU mapping");
  // The buffers go with the slot's retired objects (the GPU mapping ends when the block is destroyed, a frame or two
  // later); the region's memory is unmapped and freed after that (Cleanup).
  Video::RetireLocked(std::move(region));
  if (readback)
    Video::RetireLocked(std::move(readback));
  if (upload)
    Video::RetireLocked(std::move(upload));
}

// A committed chunk of the real guest memory: every view mapped first, then GPU-mapped and read back (never written).
void RunChunkProbe(VideoState &s, bool cached) {
  size_t offset = 0, length = 0;
  uint8_t *shadow = nullptr;
  if (!RexGmShadowFrom(kPhysicalMappingOffset + (64u << 20), &offset, &shadow, &length)) {
    Say(true, "chunk: nothing committed in guest physical memory from 64 MB on");
    return;
  }
  const size_t bytes = std::min<size_t>(length, kProbeBytes);
  const u64 physical = offset >= kPhysicalMappingOffset ? u64(offset - kPhysicalMappingOffset) : 0;
  Say(false, "chunk: mapping offset {:#x} (guest physical {:#x}), {} KiB, shadow {:#x}: {}", offset, physical, length >> 10,
      reinterpret_cast<u64>(shadow), MemoryStateOf(shadow));
  const bool views = RexGmMapAllViews(offset);
  Say(!views, "  every view of the chunk mapped first: {}", views ? "yes" : "NO (some view refused: the GPU mapping is "
                                                                             "not tried on it)");
  if (!views)
    return;
  if (physical + bytes > 0x20000000ull || (bytes & (kPage - 1)))
    return;
  // What the CPU sees now (the game's bytes, its dirty lines included), then the lines cleaned so the GPU reads the
  // same from memory. The game may write the chunk between the snapshot and the GPU's copy: a few differences then.
  std::unique_ptr<uint8_t[]> snapshot(new uint8_t[bytes]);
  std::memcpy(snapshot.get(), shadow, bytes);
  armDCacheClean(shadow, bytes);
  plume::deko3dLog("[gpuprobe] making the GPU's block over a real guest memory chunk now");
  std::unique_ptr<plume::RenderBuffer> chunk =
      plume::Deko3DCreateBufferOverMemory(s.device.get(), shadow, bytes, cached, "guest probe chunk");
  if (!chunk) {
    Say(true, "RESULT chunk: no GPU mapping of the real chunk's code alias");
    return;
  }
  Say(false, "RESULT chunk: GPU-mapped. State now: {}", MemoryStateOf(shadow));
  std::unique_ptr<plume::RenderBuffer> readback = s.device->createBuffer(
      plume::RenderBufferDesc::ReadbackBuffer(bytes, plume::RenderBufferFlag::HOST_CACHED));
  if (readback && GpuCopyNow(s, readback.get(), 0, chunk.get(), 0, bytes)) {
    const plume::RenderRange all(0, bytes);
    const uint8_t *got = static_cast<const uint8_t *>(readback->map(0, &all));
    // Against the snapshot: none differ when the GPU reads the game's memory as the CPU has it; a few are the game
    // writing meanwhile; thousands mean the GPU read something else (stale lines, a wrong mapping).
    size_t first = 0;
    const u32 differ = CountDifferentDwords(got, snapshot.get(), bytes, &first);
    // And the game's own view of the memory now, for the reader of the log (the raw physical window).
    const uint8_t *game = GuestPhysicalPtr<const uint8_t>(u32(physical));
    size_t first_now = 0;
    const u32 differ_now = CountDifferentDwords(game, snapshot.get(), bytes, &first_now);
    Say(differ > 1024, "RESULT chunk GPU read: {} of {} dwords differ from the CPU's snapshot (first at +{:#x}); the "
                       "game changed {} of them meanwhile{}",
        differ, bytes / 4, first, differ_now, differ == 0 ? ": the GPU reads the game's memory" : (differ <= differ_now ? " (so the GPU read is right)" : ""));
    readback->unmap(0, nullptr);
  }
  // Mapping a view while it is GPU-mapped: the real test of RexGmFaultIn on this chunk is mapping a view it does not
  // have; all were mapped above, so the kernel's answer comes from the region's ViewTests. Nothing written to the game.
  Video::RetireLocked(std::move(chunk));
  if (readback)
    Video::RetireLocked(std::move(readback));
}

void Cleanup() {
  Probe &p = g_probe;
  for (int k = 0; k < 2; ++k)
    if (p.view_mapped[k] && p.view[k] && p.shadow)
      UnmapView(k, "at the cleanup");
  if (p.shadow) {
    Say(false, "cleanup: region's state before unmapping: {}", MemoryStateOf(p.shadow));
    bool all = true;
    for (int i = 0; i < 2; ++i) {
      if (!p.backing[i])
        continue;
      const Result rc = svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(p.shadow) + i * kProbeHalf,
                                                  reinterpret_cast<u64>(p.backing[i]), kProbeHalf);
      Say(R_FAILED(rc), "cleanup: svcUnmapProcessCodeMemory of half {}: {:#x}{}", i, rc,
          R_FAILED(rc) ? " (the GPU mapping still holds it: its memory is left as it is)" : "");
      if (R_SUCCEEDED(rc)) {
        free(p.backing[i]);
        p.backing[i] = nullptr;
      } else {
        all = false;
      }
    }
    if (all) {
      virtmemLock();
      if (p.shadow_reservation)
        virtmemRemoveReservation(p.shadow_reservation);
      virtmemUnlock();
      p.shadow_reservation = nullptr;
      p.shadow = nullptr;
    }
  }
  for (int k = 0; k < 2; ++k) {
    if (p.view[k] && !p.view_mapped[k]) {
      virtmemLock();
      if (p.view_reservation[k])
        virtmemRemoveReservation(p.view_reservation[k]);
      virtmemUnlock();
      p.view_reservation[k] = nullptr;
      p.view[k] = nullptr;
    }
  }
}

}  // namespace
#endif

void GuestGpuProbeFrameLocked() {
#if defined(__SWITCH__) && defined(PLUME_DEKO3D)
  static const int mode = REXCVAR_GET(tw2de_diag_gpu_guest_probe);
  if (mode <= 0)
    return;
  Probe &p = g_probe;
  ++p.frames;
  if (p.phase == 0) {
    if (p.frames < uint64_t(std::max(1, REXCVAR_GET(tw2de_diag_gpu_guest_probe_frame))))
      return;
    auto &s = State();
    if (!s.device || !IsDeko3D())
      return;
    const bool cached = REXCVAR_GET(tw2de_diag_gpu_guest_probe_cached);
    Say(false, "start (mode {}, GPU {}): a 2 MB region made as a guest memory chunk is, GPU-mapped and copied both "
               "ways; a view of it kept mapped through that, another mapped, protected and unmapped meanwhile{}",
        mode, cached ? "cached" : "uncached", mode >= 2 ? "; then a chunk of the real guest memory, GPU-mapped and read" : "");
    RunRegionProbe(s, cached);
    if (mode >= 2)
      RunChunkProbe(s, cached);
    Say(false, "done; the region is unmapped and freed in a few frames");
    p.phase = 1;
    p.done_at = p.frames;
    return;
  }
  if (p.phase == 1 && p.frames >= p.done_at + 8) {
    Cleanup();
    p.phase = 2;
  }
#endif
}

}  // namespace renut::gpu

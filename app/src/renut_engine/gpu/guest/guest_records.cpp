/**
 * @file    gpu/guest/guest_records.cpp
 */
#include "renut_engine/gpu/guest/guest_records.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <o1heap.h>
#include <rex/system/xmemory.h>

#include "renut_engine/gpu/log.h"

namespace renut::gpu::records {

namespace {

// Records are small (headers + a few host pointers); 64 MiB is ample.
// tw2de (2026-11-11): the most it takes; with less guest physical memory left (the game's render target and
// texture pool grown for a 1600x900 or 1920x1080 scene: tw2de_render_resolution.cpp) 48, 32, 24 or 16 MiB, the
// first that can be reserved (HeapStats: its peak use, logged with the pool's).
constexpr u32 kHeapSize = 64u << 20;
u32 g_heap_size = 0;

std::mutex g_heap_mutex;  // o1heap is not internally synchronised
O1HeapInstance *g_heap = nullptr;
bool g_oom_logged = false;

// tw2de (2026-11-06, seen19): which addresses of the record heap hold a record, one byte (kind + 1, 0 = none) per
// heap alignment unit, read without a lock. It was a std::shared_mutex around an unordered_map: run 751caa1c, at an
// area entry the render thread spent 16.9 ms of one frame in the draws' shader and declaration lookups (FindShaderLocked
// -> FindRecord -> Lookup: a shared lock and a hash lookup each), and ran only 68 of its 92 busy ms, while the game and
// its loaders made records (Register: the exclusive lock) as fast as they could. libnx's rwlock waits on a condition
// variable, without priority inheritance: a writer the scheduler held back held the render thread back with it. Every
// record comes from the heap (CreateRecord), whose blocks start at multiples of the alignment unit. Anything outside
// the heap is not ours, as before.
constexpr u32 kUnit = 16;  // o1heap's alignment (O1HEAP_ALIGNMENT: 2 pointers)
u32 g_heap_va = 0;
std::atomic<uint8_t> *g_kinds = nullptr;  // kHeapSize / kUnit entries
std::atomic<uint64_t> g_generation{0};

std::atomic<uint8_t> *KindSlot(u32 guest_va) {
  if (!g_kinds || guest_va < g_heap_va || guest_va - g_heap_va >= g_heap_size || (guest_va - g_heap_va) % kUnit)
    return nullptr;
  return &g_kinds[(guest_va - g_heap_va) / kUnit];
}

} // namespace

u32 AllocPhysicalBlock(u32 size, bool quiet) {
  // The physical window (64 KiB pages) maps host and guest addresses 1:1 in
  // offset, so a host pointer into it round-trips to a guest VA.
  rex::memory::BaseHeap *heap = REX_KERNEL_MEMORY()->LookupHeapByType(true, 64u * 1024u);
  if (!heap) {
    RGPU_ERROR("No guest physical heap");
    return 0;
  }
  u32 guest_va = 0;
  if (!heap->Alloc(size, 64u * 1024u,
                   rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, true,
                   &guest_va)) {
    if (!quiet)
      RGPU_ERROR("Reserving {} bytes of guest physical memory failed", size);
    return 0;
  }
  return guest_va;
}

bool Init() {
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    return true;

  auto *memory = REX_KERNEL_MEMORY();
  u32 guest_va = 0, size = 0;
  for (const u32 mb : {64u, 48u, 32u, 24u, 16u}) {
    size = mb << 20;
    if ((guest_va = AllocPhysicalBlock(size, true)) != 0)
      break;
  }
  if (!guest_va) {
    RGPU_ERROR("Record heap: reserving even {} bytes failed", size);
    return false;
  }
  g_heap_size = size;
  g_heap = o1heapInit(memory->TranslateVirtual<void *>(guest_va), size);
  if (!g_heap) {
    RGPU_ERROR("Record heap: o1heapInit failed");
    return false;
  }
  // tw2de (2026-11-06, seen19): the registry (4 MiB), before the first record.
  static_assert(O1HEAP_ALIGNMENT % kUnit == 0, "records start at multiples of kUnit");
  g_kinds = new std::atomic<uint8_t>[kHeapSize / kUnit]();
  g_heap_va = guest_va;
  return true;
}

void *AllocRaw(size_t size) {
  std::lock_guard lock(g_heap_mutex);
  if (!g_heap) {
    RGPU_ERROR("Record heap used before Init");
    return nullptr;
  }
  void *p = o1heapAllocate(g_heap, size ? size : 1);
  if (!p && !g_oom_logged) {
    RGPU_ERROR("Record heap exhausted ({} bytes requested)", size);
    g_oom_logged = true;
  }
  return p;
}

void FreeRaw(void *host) {
  if (!host)
    return;
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    o1heapFree(g_heap, host);
}

void Register(u32 guest_va, RecordKind kind) {
  if (auto *slot = KindSlot(guest_va)) {
    slot->store(uint8_t(u32(kind) + 1), std::memory_order_release);
    return;
  }
  RGPU_ERROR("Record at {:08X} outside the record heap: not registered", guest_va);
}

void Unregister(u32 guest_va) {
  if (auto *slot = KindSlot(guest_va))
    slot->store(0, std::memory_order_release);
  g_generation.fetch_add(1, std::memory_order_release);
}

bool Lookup(u32 guest_va, RecordKind *out_kind) {
  std::atomic<uint8_t> *slot = KindSlot(guest_va);
  const uint8_t v = slot ? slot->load(std::memory_order_acquire) : 0;
  if (!v)
    return false;
  *out_kind = RecordKind(v - 1);
  return true;
}

void HeapStats(size_t *capacity, size_t *peak, uint64_t *oom) {
  std::lock_guard lock(g_heap_mutex);
  if (!g_heap) {
    *capacity = *peak = 0;
    *oom = 0;
    return;
  }
  const O1HeapDiagnostics d = o1heapGetDiagnostics(g_heap);
  *capacity = d.capacity;
  *peak = d.peak_allocated;
  *oom = d.oom_count;
}

uint64_t Generation() {
  return g_generation.load(std::memory_order_acquire);
}


} // namespace renut::gpu::records

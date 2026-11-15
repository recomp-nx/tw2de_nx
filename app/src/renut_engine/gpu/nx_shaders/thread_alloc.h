// tw2de (2026-10-21): per-thread allocation for the shader pre-pass's uam threads. newlib's malloc has one lock
// for the whole process, and a uam compile makes ~670,000 malloc/free calls (Mesa's ralloc, nv50_ir's objects): three
// compile threads spent most of their time waiting for each other there (the console's CPU was not busy at 30 shaders
// a minute). A thread that opts in (ThreadAllocBegin) takes its blocks (up to 512 KB since 2026-10-21; 4 KB before) from 1 MB slabs of its
// own, with free lists per size class, and no lock; everything else is newlib's, as before.
//
// malloc / free / realloc / calloc are wrapped at link time (-Wl,--wrap=..., cmake/tw2de_renut_gpu.cmake). A thread
// that has not opted in pays one load of a global flag; free() of a pointer pays one more (a slab map lookup) only
// while slabs exist. A block freed by another thread than its owner is not reused (rare: Mesa's process-wide tables).
// Slabs with no live block left are returned to newlib when their thread ends its use (ThreadAllocEnd); one still
// holding blocks then (Mesa's process-wide tables, a result another thread frees later) goes when its last block is
// freed (2026-10-22; before, it stayed for good).
#pragma once

#include <cstddef>
#include <cstdint>

namespace renut::nxs {

void ThreadAllocBegin();
void ThreadAllocEnd();

// Only while one of these is alive does the thread allocate from its own slabs (frees of its blocks go back to it
// at any time). Around uam::compile alone: what the thread keeps (the store's index, results) stays newlib's, so no
// long-lived block pins a slab after the pre-pass.
struct ThreadAllocScope {
  ThreadAllocScope();
  ~ThreadAllocScope();
};

// tw2de (2026-10-21): the same as a ThreadAllocScope's life, for code that cannot hold one (the pre-pass's
// translations: PrepassConfig::translate_scope).
void ThreadAllocSetActive(bool active);

// tw2de (2026-11-08): the most memory the calling thread held during its last ThreadAllocScope (its newlib
// blocks' peak plus the slabs it used), in bytes: a compile's own peak.
uint64_t ThreadAllocScopePeak();

struct ThreadAllocStats {
  uint64_t slabs_live, slab_bytes, blocks_large, frees_remote;
  // tw2de (2026-11-10): the aligned allocations (memalign above 16) that went to newlib, by why: outside a scope,
  // an alignment above 4 KB, not a power of two, too large; and the largest alignment seen and its size.
  uint64_t align_inactive, align_big, align_odd, align_large, align_max, align_max_size;
};
ThreadAllocStats GetThreadAllocStats();

// tw2de (2026-10-26): every heap block of 256 KB or more, by where it was allocated (the call stack's first frames,
// as addresses, named by the memory report from <nro>.sym). Off until HeapSitesEnable (log_diagnostics, at
// the app's start): then such an allocation also walks its stack (a few microseconds; they are rare) and its free a
// table lookup. HeapSitesReport hands each site's live bytes, block count and frames (kHeapSiteFrames, 0 = none),
// largest first, to 'site', up to 'max' sites; it returns the live bytes of all of them.
constexpr unsigned kHeapSiteFrames = 6;
void HeapSitesEnable();
uint64_t HeapSitesReport(void (*site)(uint64_t bytes, uint32_t count, const uintptr_t *frames, void *arg), void *arg,
                         unsigned max);

}  // namespace renut::nxs

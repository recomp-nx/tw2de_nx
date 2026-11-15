// tw2de (2026-10-21): thread_alloc.h.
#include "renut_engine/gpu/nx_shaders/thread_alloc.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"
#include "renut_engine/gpu/nx_shaders/thread_waits.h"

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include <malloc.h>
#include <cstdio>
#include <unwind.h>

// tw2de (2026-10-22): under AddressSanitizer (the PC harness), the slabs' free memory is poisoned: a read or write
// of a freed block (or of a slab's memory not handed out yet) is reported as for newlib's blocks.
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define TA_POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define TA_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define TA_POISON(p, n) ((void)(p), (void)(n))
#define TA_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

extern "C" {
int __real_pthread_mutex_lock(pthread_mutex_t *m);
struct _reent;
void __real___malloc_lock(struct _reent *r);
void *__real_malloc(size_t size);
void __real_free(void *ptr);
void *__real_realloc(void *ptr, size_t size);
void *__real_calloc(size_t count, size_t size);
}

namespace renut::nxs {

namespace {

constexpr size_t kSlabShift = 20;  // 1 MB slabs, aligned to their size: a block's slab is its address >> 20
constexpr size_t kSlabSize = size_t(1) << kSlabShift;
constexpr size_t kHeader = 16;     // before each block: its size class (keeps 16-byte alignment)
// tw2de (2026-10-21): blocks up to 512 KB (was 4 KB). The console's log: the ~2,100 newlib calls a compile still
// made (Mesa's and nv50_ir's 4-512 KB arrays, the reallocs growing past 4 KB) waited ~1 s a shader for newlib's one
// lock on cores 1 and 2 (core 0's thread got it back each time). A uam compile makes ~440 such blocks and ~3 larger.
constexpr size_t kMaxSmall = 512u << 10;
constexpr uint32_t kClasses = 37;
// Block sizes with their header: 32 .. 4112 bytes about 1.5x apart, then 1.5x / 2x steps to 512 KB + 16 (a block
// never spans slabs: the largest is half a slab).
constexpr uint32_t kClassSize[kClasses] = {
    32,     48,     64,     80,     96,      128,     160,     192,     256,     320,    384,    512,   640,
    768,    1024,   1280,   1536,   2048,    3072,    4112,    6160,    8208,    12304,  16400,  24592, 32784,
    49168,  65552,  98320,  131088, 196624,  262160,  327696,  393232,  458768,  491536, 524304};

uint32_t ClassOf(size_t total) {
  uint32_t c = 0;
  while (kClassSize[c] < total)
    ++c;
  return c;
}

struct Cache;

struct Slab {  // at the start of each slab
  std::atomic<Cache *> owner;
  std::atomic<int64_t> live;  // blocks handed out and not freed
  std::atomic<uint32_t> released;  // given back to newlib (once: whoever sets it)
  uint32_t pad32;
  uint64_t pad[5];
};
static_assert(sizeof(Slab) % 16 == 0, "blocks stay 16-byte aligned");

struct Cache {
  void *free_list[kClasses] = {};
  char *bump = nullptr, *bump_end = nullptr;
  Slab *slabs[16384] = {};
  uint32_t slab_count = 0;
  // tw2de (2026-11-08): empty slabs kept by the last trim (CacheTrim), taken again before newlib is asked.
  Slab *spare[32] = {};
  uint32_t spare_count = 0;
};
// 32: a compile takes ~15 of them (the pre-pass log: 32-48 slabs live over three threads); fewer kept meant newlib's
// aligned 1 MB allocations every compile, and its arena grew with the holes (300 -> 680 MB over 20,000 compiles).
constexpr uint32_t kSpareSlabs = 32;

// The slabs, by address >> 22, in an open-addressing table (a key of 0 is empty, ~0 a removed one).
constexpr uint32_t kTable = 65536;
std::atomic<uint64_t> g_table[kTable];
std::atomic<uint32_t> g_slabs_live{0};
std::atomic<uint32_t> g_threads_on{0};
NxMutex g_table_mutex;
std::atomic<uint64_t> g_blocks_large{0}, g_frees_remote{0};
std::atomic<uint64_t> g_align_inactive{0}, g_align_big{0}, g_align_odd{0}, g_align_large{0}, g_align_max{0},
    g_align_max_size{0};

static_assert(kClassSize[kClasses - 1] >= kMaxSmall + 16, "the largest class takes kMaxSmall");
static_assert(kClassSize[kClasses - 1] <= (size_t(1) << 20) / 2 + 16, "half a slab at most");

inline uint32_t Hash(uint64_t key) { return uint32_t((key * 0x9E3779B97F4A7C15ull) >> 52) & (kTable - 1); }

bool IsSlab(uint64_t key) {
  for (uint32_t i = Hash(key), n = 0; n < kTable; ++n, i = (i + 1) & (kTable - 1)) {
    const uint64_t k = g_table[i].load(std::memory_order_acquire);
    if (k == key)
      return true;
    if (k == 0)
      return false;
  }
  return false;
}

void AddSlab(uint64_t key) {
  std::lock_guard<NxMutex> lock(g_table_mutex);
  for (uint32_t i = Hash(key), n = 0; n < kTable; ++n, i = (i + 1) & (kTable - 1)) {
    const uint64_t k = g_table[i].load(std::memory_order_relaxed);
    if (k == 0 || k == ~uint64_t(0)) {
      g_table[i].store(key, std::memory_order_release);
      g_slabs_live.fetch_add(1, std::memory_order_release);
      return;
    }
  }
}

void RemoveSlab(uint64_t key) {
  std::lock_guard<NxMutex> lock(g_table_mutex);
  for (uint32_t i = Hash(key), n = 0; n < kTable; ++n, i = (i + 1) & (kTable - 1)) {
    const uint64_t k = g_table[i].load(std::memory_order_relaxed);
    if (k == key) {
      g_table[i].store(~uint64_t(0), std::memory_order_release);  // a removed one: lookups go on past it
      g_slabs_live.fetch_sub(1, std::memory_order_release);
      return;
    }
    if (k == 0)
      return;
  }
}

thread_local Cache *t_cache = nullptr;
thread_local bool t_active = false;
// tw2de (2026-11-08): what a thread with a cache holds of newlib's heap (its newlib allocations less its frees of
// newlib blocks), the most of it since its scope began, and the slabs its last scope used: a compile's peak
// (ThreadAllocScopePeak).
thread_local int64_t t_newlib_now = 0, t_newlib_base = 0, t_newlib_peak = 0;
thread_local uint32_t t_scope_slabs = 0;
inline void NewlibAdd(int64_t bytes) {
  t_newlib_now += bytes;
  if (t_newlib_now > t_newlib_peak)
    t_newlib_peak = t_newlib_now;
}

// tw2de (2026-11-10): the slabs from 64 MB chunks of newlib's heap, a freed slab kept for the next one (a chunk
// back to newlib once all its slabs are free, and another empty chunk's worth is kept). Before, each slab was newlib's
// aligned_alloc(1 MB, 1 MB): it carves ~2 MB and the freed slab's exact 1 MB hole never holds another aligned slab, so
// the heap became such holes: the pre-pass's scan had 1,950 MB free and newlib refused 580,000 slabs in 80 s (every
// block then from newlib, under its one lock: the cores idle).
constexpr size_t kChunkSlabs = 64;
constexpr size_t kChunkSize = kChunkSlabs * kSlabSize;
constexpr size_t kMaxChunks = 64;  // 4 GB: more than the process has
struct Chunk {
  char *base;
  uint32_t used;  // slabs handed out
};
NxMutex g_pool_mutex;
Chunk g_chunks[kMaxChunks];
size_t g_chunk_count = 0;
void *g_pool_free[kMaxChunks * kChunkSlabs];
size_t g_pool_free_count = 0;

Chunk *ChunkOf(const void *p) {
  for (size_t i = 0; i < g_chunk_count; ++i)
    if (static_cast<const char *>(p) >= g_chunks[i].base && static_cast<const char *>(p) < g_chunks[i].base + kChunkSize)
      return &g_chunks[i];
  return nullptr;
}

void *PoolGet() {
  std::lock_guard<NxMutex> lock(g_pool_mutex);
  if (!g_pool_free_count) {
    if (g_chunk_count == kMaxChunks)
      return nullptr;
    char *base = static_cast<char *>(aligned_alloc(kSlabSize, kChunkSize));  // not wrapped: newlib's
    if (!base)
      return nullptr;
    g_chunks[g_chunk_count++] = {base, 0};
    for (size_t i = kChunkSlabs; i-- > 0;)
      g_pool_free[g_pool_free_count++] = base + i * kSlabSize;
  }
  void *slab = g_pool_free[--g_pool_free_count];
  ChunkOf(slab)->used++;
  return slab;
}

void PoolPut(void *slab) {
  std::lock_guard<NxMutex> lock(g_pool_mutex);
  Chunk *c = ChunkOf(slab);
  if (!c) {
    __real_free(slab);  // not the pool's (cannot be: every slab is)
    return;
  }
  g_pool_free[g_pool_free_count++] = slab;
  if (--c->used != 0 || g_pool_free_count < 2 * kChunkSlabs)
    return;
  // The chunk is empty and another chunk's worth of free slabs stays: the chunk back to newlib.
  char *base = c->base;
  size_t keep = 0;
  for (size_t i = 0; i < g_pool_free_count; ++i)
    if (static_cast<char *>(g_pool_free[i]) < base || static_cast<char *>(g_pool_free[i]) >= base + kChunkSize)
      g_pool_free[keep++] = g_pool_free[i];
  g_pool_free_count = keep;
  *c = g_chunks[--g_chunk_count];
  __real_free(base);
}

bool NewSlab(Cache &c) {
  if (c.slab_count == 16384)
    return false;
  if (c.spare_count) {  // tw2de (2026-11-08): an empty one the last trim kept (still registered, still ours)
    Slab *slab = c.spare[--c.spare_count];
    c.slabs[c.slab_count++] = slab;
    c.bump = reinterpret_cast<char *>(slab) + sizeof(Slab);
    c.bump_end = reinterpret_cast<char *>(slab) + kSlabSize;
    TA_POISON(c.bump, size_t(c.bump_end - c.bump));
    return true;
  }
  void *mem = PoolGet();
  if (!mem) {
    MyWaits().slab_fail++;
    return false;
  }
  Slab *slab = new (mem) Slab{};
  slab->owner.store(&c, std::memory_order_release);
  c.slabs[c.slab_count++] = slab;
  c.bump = static_cast<char *>(mem) + sizeof(Slab);
  c.bump_end = static_cast<char *>(mem) + kSlabSize;
  TA_POISON(c.bump, size_t(c.bump_end - c.bump));
  AddSlab(reinterpret_cast<uintptr_t>(mem) >> kSlabShift);
  return true;
}

inline Slab *SlabOf(const void *p) {
  return reinterpret_cast<Slab *>(reinterpret_cast<uintptr_t>(p) & ~(uintptr_t(kSlabSize) - 1));
}

// Ours: a pointer in a registered slab.
inline bool Ours(const void *p) {
  if (!p || g_slabs_live.load(std::memory_order_acquire) == 0)
    return false;
  return IsSlab(reinterpret_cast<uintptr_t>(p) >> kSlabShift);
}

// tw2de (2026-10-22): a slab nobody owns any more goes back to newlib when its last block is freed (before, it
// stayed: the run-time uam thread's idle ends left a few behind every time).
void ReleaseSlab(Slab *slab) {
  if (slab->released.exchange(1, std::memory_order_acq_rel) != 0)
    return;
  RemoveSlab(reinterpret_cast<uintptr_t>(slab) >> kSlabShift);
  PoolPut(slab);
}

// tw2de (2026-11-10): an aligned block (memalign above 16 bytes) is a larger block with the pointer moved up;
// the header before the moved pointer holds kOffsetMark | the distance back to the block's own pointer.
constexpr uint32_t kOffsetMark = 0x80000000u;

inline void *BlockStart(void *p) {
  uint32_t c;
  std::memcpy(&c, static_cast<const char *>(p) - kHeader, sizeof(c));
  return (c & kOffsetMark) ? static_cast<char *>(p) - (c & ~kOffsetMark) : p;
}

inline size_t UsableSize(const void *p) {
  uint32_t c;
  std::memcpy(&c, static_cast<const char *>(p) - kHeader, sizeof(c));
  if (c & kOffsetMark) {
    const size_t off = c & ~kOffsetMark;
    const char *start = static_cast<const char *>(p) - off;
    std::memcpy(&c, start - kHeader, sizeof(c));
    return kClassSize[c] - kHeader - off;
  }
  return kClassSize[c] - kHeader;
}

void *CacheAlloc(Cache &c, size_t size) {
  const uint32_t cls = ClassOf(size + kHeader);
  char *block = static_cast<char *>(c.free_list[cls]);
  if (block) {
    TA_UNPOISON(block, kHeader);
    void *next;
    std::memcpy(&next, block, sizeof(next));
    c.free_list[cls] = next;
  }
  else {
    const size_t need = kClassSize[cls];
    if (size_t(c.bump_end - c.bump) < need && !NewSlab(c))
      return nullptr;
    block = c.bump;
    c.bump += need;
  }
  TA_UNPOISON(block, kClassSize[cls]);  // the whole class: realloc copies a block's usable size
  std::memcpy(block, &cls, sizeof(cls));
  SlabOf(block)->live.fetch_add(1, std::memory_order_relaxed);
  return block + kHeader;
}

void CacheFree(void *p) {
  p = BlockStart(p);
  char *block = static_cast<char *>(p) - kHeader;
  Slab *slab = SlabOf(block);
#if defined(__SANITIZE_ADDRESS__)
  {
    uint32_t cls0;
    std::memcpy(&cls0, block, sizeof(cls0));
    TA_POISON(block + kHeader, kClassSize[cls0] - kHeader);
  }
#endif
  Cache *c = t_cache;
  if (c && slab->owner.load(std::memory_order_relaxed) == c) {
    slab->live.fetch_sub(1, std::memory_order_relaxed);
    uint32_t cls;
    std::memcpy(&cls, block, sizeof(cls));
    void *head = c->free_list[cls];
    std::memcpy(block, &head, sizeof(head));
    c->free_list[cls] = block;
    TA_POISON(block, kHeader);
  }
  else {
    g_frees_remote.fetch_add(1, std::memory_order_relaxed);  // not reused; the slab goes once nothing in it lives
    if (slab->live.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        slab->owner.load(std::memory_order_acquire) == nullptr)
      ReleaseSlab(slab);
  }
}

// tw2de (2026-11-08): the end of a scope (a compile): the slabs back to newlib. The free lists are per size class
// and a slab is only ever bump-allocated once, so what a thread held grew to the sum of every class's largest use
// over all the compiles it made (each shader's peak in other classes than the last's), never given back until the
// thread ended: a full pre-pass (20,000 compiles) ran out of memory in uam near its end, three times in a row
// (std::bad_alloc in nv50_ir's register allocation). Now each compile starts from empty slabs: the free lists are
// dropped, an empty slab is kept (up to kSpareSlabs) or released, and one still holding blocks (what outlives the compile:
// Mesa's process-wide tables, a result) is given up as ThreadAllocEnd does (owned by nobody, released when its last
// block is freed; its free blocks are not reused).
void CacheTrim(Cache &c) {
  t_scope_slabs = c.slab_count;  // tw2de (2026-11-08): ThreadAllocScopePeak
  for (void *&head : c.free_list)
    head = nullptr;
  c.bump = c.bump_end = nullptr;
  for (uint32_t i = 0; i < c.slab_count; ++i) {
    Slab *slab = c.slabs[i];
    if (slab->live.load(std::memory_order_acquire) == 0) {
      if (c.spare_count < kSpareSlabs) {
        c.spare[c.spare_count++] = slab;
        continue;
      }
      slab->owner.store(nullptr, std::memory_order_release);
      ReleaseSlab(slab);
      continue;
    }
    slab->owner.store(nullptr, std::memory_order_release);
    // A block freed between the load above and the store (another thread's free) found the owner still set and did
    // not release it: look again.
    if (slab->live.load(std::memory_order_acquire) == 0)
      ReleaseSlab(slab);
  }
  c.slab_count = 0;
}

}  // namespace

void ThreadAllocBegin() {
  if (t_cache)
    return;
  t_cache = new (__real_malloc(sizeof(Cache))) Cache{};
  g_threads_on.fetch_add(1, std::memory_order_release);
}

ThreadAllocScope::ThreadAllocScope() {
  t_active = t_cache != nullptr;
  t_newlib_base = t_newlib_peak = t_newlib_now;  // tw2de (2026-11-08)
  t_scope_slabs = 0;
}

uint64_t ThreadAllocScopePeak() {
  const int64_t newlib = t_newlib_peak - t_newlib_base;
  return uint64_t(newlib > 0 ? newlib : 0) + uint64_t(t_scope_slabs) * kSlabSize;
}
ThreadAllocScope::~ThreadAllocScope() {
  t_active = false;
  if (t_cache)
    CacheTrim(*t_cache);  // tw2de (2026-11-08)
}

void ThreadAllocSetActive(bool active) {
  const bool was = t_active;
  t_active = active && t_cache != nullptr;
  if (was && !t_active && t_cache)
    CacheTrim(*t_cache);  // tw2de (2026-11-08): the end of a translation's scope too
}

void ThreadAllocEnd() {
  Cache *c = t_cache;
  if (!c)
    return;
  t_cache = nullptr;
  t_active = false;
  g_threads_on.fetch_sub(1, std::memory_order_release);
  for (uint32_t i = 0; i < c->spare_count; ++i) {  // tw2de (2026-11-08): the empty ones a trim kept
    c->spare[i]->owner.store(nullptr, std::memory_order_release);
    ReleaseSlab(c->spare[i]);
  }
  for (uint32_t i = 0; i < c->slab_count; ++i) {
    Slab *slab = c->slabs[i];
    // A slab still holding live blocks (Mesa's process-wide tables made during a compile) stays, owned by nobody:
    // its blocks are freed by whoever frees them, and never reused.
    slab->owner.store(nullptr, std::memory_order_release);
    if (slab->live.load(std::memory_order_acquire) == 0)
      ReleaseSlab(slab);
  }
  __real_free(c);
}

ThreadAllocStats GetThreadAllocStats() {
  ThreadAllocStats s{};
  s.slabs_live = g_slabs_live.load();
  s.slab_bytes = s.slabs_live * kSlabSize;
  s.blocks_large = g_blocks_large.load();
  s.frees_remote = g_frees_remote.load();
  s.align_inactive = g_align_inactive.load();
  s.align_big = g_align_big.load();
  s.align_odd = g_align_odd.load();
  s.align_large = g_align_large.load();
  s.align_max = g_align_max.load();
  s.align_max_size = g_align_max_size.load();
  return s;
}

}  // namespace renut::nxs

using namespace renut::nxs;

namespace {
// newlib's heap calls of a pre-pass thread (one with a cache), timed (thread_waits.h).
struct HeapTimer {
  uint64_t t0 = 0;
  HeapTimer() {
    if (t_cache)
      t0 = WaitTicks();
  }
  ~HeapTimer() {
    if (t0) {
      ThreadWaits &w = MyWaits();
      w.heap += WaitTicks() - t0;
      w.heap_calls++;
    }
  }
};
}  // namespace

// tw2de (2026-10-26): HeapSitesEnable / HeapSitesReport (thread_alloc.h). A fixed table of live big blocks
// (pointer -> size, site) and one of sites (frames -> live bytes, count), made once with newlib's malloc: the tracking
// itself never allocates through the wrappers. One spin lock (big allocations are rare).
namespace {
constexpr size_t kBigBlock = 256u << 10;
constexpr unsigned kFrames = renut::nxs::kHeapSiteFrames, kSkipFrames = 2;  // the wrapper and Track themselves
constexpr size_t kLiveSlots = size_t(1) << 16, kSiteSlots = 2048;
struct LiveBlock { uintptr_t ptr; uint64_t size; uint32_t site; };
struct Site { uintptr_t frames[kFrames]; uint64_t bytes; uint32_t count; bool used; };
LiveBlock *g_live = nullptr;
Site *g_sites = nullptr;
std::atomic<bool> g_sites_on{false};
std::atomic_flag g_sites_lock = ATOMIC_FLAG_INIT;
thread_local bool t_tracking = false;

struct SitesLock {
  SitesLock() { while (g_sites_lock.test_and_set(std::memory_order_acquire)) {} }
  ~SitesLock() { g_sites_lock.clear(std::memory_order_release); }
};

struct Walk { uintptr_t frames[kFrames + kSkipFrames]; unsigned n; };
_Unwind_Reason_Code WalkFrame(struct _Unwind_Context *ctx, void *arg) {
  Walk &w = *static_cast<Walk *>(arg);
  if (w.n >= kFrames + kSkipFrames)
    return _URC_END_OF_STACK;
  w.frames[w.n++] = uintptr_t(_Unwind_GetIP(ctx));
  return _URC_NO_REASON;
}

void Track(void *p, size_t size) {
  if (!p || size < kBigBlock || !g_sites_on.load(std::memory_order_relaxed) || t_tracking || Ours(p))
    return;  // a slab's block (Ours) goes back to its slab, not through Untrack
  t_tracking = true;  // the unwinder may allocate (its first use sorts the frame tables)
  Walk w{};
  _Unwind_Backtrace(WalkFrame, &w);
  uintptr_t frames[kFrames] = {};
  for (unsigned i = 0; i < kFrames && i + kSkipFrames < w.n; ++i)
    frames[i] = w.frames[i + kSkipFrames];  // addresses (the report finds the NRO's base)
  {
    SitesLock lock;
    uint64_t h = 1469598103934665603ull;
    for (uintptr_t f : frames)
      h = (h ^ f) * 1099511628211ull;
    uint32_t site = uint32_t(h % kSiteSlots);
    for (size_t probe = 0; probe < kSiteSlots; ++probe, site = (site + 1) % kSiteSlots) {
      Site &s = g_sites[site];
      if (!s.used) {
        s.used = true;
        std::memcpy(s.frames, frames, sizeof(frames));
        break;
      }
      if (std::memcmp(s.frames, frames, sizeof(frames)) == 0)
        break;
    }
    g_sites[site].bytes += size;
    g_sites[site].count++;
    size_t slot = (reinterpret_cast<uintptr_t>(p) >> 4) % kLiveSlots;
    for (size_t probe = 0; probe < kLiveSlots; ++probe, slot = (slot + 1) % kLiveSlots) {
      if (g_live[slot].ptr == 0 || g_live[slot].ptr == 1) {  // empty or a freed slot
        g_live[slot] = {reinterpret_cast<uintptr_t>(p), size, site};
        break;
      }
    }
  }
  t_tracking = false;
}

void Untrack(void *p) {
  if (!p || !g_sites_on.load(std::memory_order_relaxed) || malloc_usable_size(p) < kBigBlock)
    return;
  SitesLock lock;
  size_t slot = (reinterpret_cast<uintptr_t>(p) >> 4) % kLiveSlots;
  for (size_t probe = 0; probe < kLiveSlots; ++probe, slot = (slot + 1) % kLiveSlots) {
    LiveBlock &b = g_live[slot];
    if (b.ptr == 0)
      return;  // allocated before the tracking started
    if (b.ptr == reinterpret_cast<uintptr_t>(p)) {
      g_sites[b.site].bytes -= b.size;
      g_sites[b.site].count--;
      b.ptr = 1;  // a freed slot: probing goes on past it
      return;
    }
  }
}
}  // namespace

namespace renut::nxs {

void HeapSitesEnable() {
  if (g_sites_on.load())
    return;
  g_live = static_cast<LiveBlock *>(__real_calloc(kLiveSlots, sizeof(LiveBlock)));
  g_sites = static_cast<Site *>(__real_calloc(kSiteSlots, sizeof(Site)));
  if (g_live && g_sites)
    g_sites_on.store(true);
}

uint64_t HeapSitesReport(void (*site)(uint64_t bytes, uint32_t count, const uintptr_t *frames, void *arg), void *arg,
                         unsigned max) {
  if (!g_sites_on.load())
    return 0;
  static Site copy[kSiteSlots];  // not on the stack (and no allocation)
  {
    SitesLock lock;
    std::memcpy(copy, g_sites, sizeof(copy));
  }
  uint64_t total = 0;
  for (const Site &s : copy)
    if (s.used)
      total += s.bytes;
  for (unsigned n = 0; n < max; ++n) {
    Site *best = nullptr;
    for (Site &s : copy)
      if (s.used && s.bytes && (!best || s.bytes > best->bytes))
        best = &s;
    if (!best)
      break;
    site(best->bytes, best->count, best->frames, arg);
    best->bytes = 0;
  }
  return total;
}


}  // namespace renut::nxs

extern "C" {

// newlib's one heap lock: how long a pre-pass thread waits for it.
void __wrap___malloc_lock(struct _reent *r) {
  if (!t_cache) {
    __real___malloc_lock(r);
    return;
  }
  const uint64_t t0 = WaitTicks();
  __real___malloc_lock(r);
  MyWaits().heap_lock += WaitTicks() - t0;
}

// Mesa's tables and libstdc++'s locks: the waits of a contended one counted.
int __wrap_pthread_mutex_lock(pthread_mutex_t *m) {
  if (!t_cache || pthread_mutex_trylock(m) != 0) {
    if (!t_cache)
      return __real_pthread_mutex_lock(m);
    const uint64_t t0 = WaitTicks();
    const int r = __real_pthread_mutex_lock(m);
    ThreadWaits &w = MyWaits();
    w.pthread_lock += WaitTicks() - t0;
    w.pthread_waits++;
    return r;
  }
  return 0;
}

// tw2de (2026-10-21): newlib's own entry points are wrapped too (_malloc_r and co.: libc's strdup, printf and
// the like allocate through them, not through malloc). newlib's malloc / realloc / calloc / memalign call _malloc_r
// and _free_r themselves and then use the chunk's header: while a thread is inside newlib (t_in_newlib) its
// _malloc_r / _free_r are newlib's, untouched.
static thread_local int t_in_newlib = 0;
struct InNewlib {
  InNewlib() { ++t_in_newlib; }
  ~InNewlib() { --t_in_newlib; }
};

static inline bool Active() { return g_threads_on.load(std::memory_order_relaxed) != 0 && t_active && t_cache; }

static void *Alloc(size_t size, bool libc) {
  if (Active()) {
    if (size <= kMaxSmall) {
      if (void *p = CacheAlloc(*t_cache, size))
        return p;
    }
    else {
      g_blocks_large.fetch_add(1, std::memory_order_relaxed);
      MyWaits().heap_big++;
    }
  }
  else if (t_cache) {
    MyWaits().heap_inactive++;
  }
  if (libc && t_cache)
    MyWaits().heap_libc++;
  HeapTimer timer;
  InNewlib in;
  void *p = __real_malloc(size);
  if (p && t_cache)
    NewlibAdd(int64_t(malloc_usable_size(p)));  // tw2de (2026-11-08)
  return p;
}

static void Free(void *p) {
  if (Ours(p)) {
    CacheFree(p);
    return;
  }
  if (!p)
    return;
  Untrack(p);  // tw2de (2026-10-26)
  if (t_cache) {
    MyWaits().heap_free_foreign++;
    NewlibAdd(-int64_t(malloc_usable_size(p)));  // tw2de (2026-11-08)
  }
  HeapTimer timer;
  InNewlib in;
  __real_free(p);
}

void *__wrap_malloc(size_t size) {
  void *p = Alloc(size, false);
  Track(p, size);  // tw2de (2026-10-26)
  return p;
}
void __wrap_free(void *p) { Free(p); }

void *__wrap_calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size)
    return nullptr;
  const size_t total = count * size;
  if (Active() && total <= kMaxSmall) {
    void *p = Alloc(total, false);
    if (p)
      std::memset(p, 0, total);
    return p;
  }
  if (t_cache)
    MyWaits().heap_inactive++;
  HeapTimer timer;
  void *q;
  {
    InNewlib in;
    q = __real_calloc(count, size);
  }
  if (q && t_cache)
    NewlibAdd(int64_t(malloc_usable_size(q)));  // tw2de (2026-11-08)
  Track(q, total);  // tw2de (2026-10-26)
  return q;
}

void *__wrap_realloc(void *p, size_t size) {
  if (Ours(p)) {
    const size_t usable = UsableSize(p);
    if (size <= usable && size > usable / 2)
      return p;
    if (size == 0) {
      CacheFree(p);
      return nullptr;
    }
    void *q = Alloc(size, false);
    if (!q)
      return nullptr;
    std::memcpy(q, p, size < usable ? size : usable);
    CacheFree(p);
    return q;
  }
  if (!p)
    return Alloc(size, false);
  if (t_cache)
    MyWaits().heap_realloc_foreign++;
  HeapTimer timer;
  Untrack(p);  // tw2de (2026-10-26): the block as it was; the new one tracked below
  const int64_t old_size = t_cache ? int64_t(malloc_usable_size(p)) : 0;
  void *q;
  {
    InNewlib in;
    q = __real_realloc(p, size);
  }
  if (t_cache && (q || !size)) {  // tw2de (2026-11-08)
    NewlibAdd(-old_size);
    if (q)
      NewlibAdd(int64_t(malloc_usable_size(q)));
  }
  Track(q, size);
  return q;
}

// newlib's reentrant entry points, as libc's own code calls them.
void *__real__malloc_r(struct _reent *r, size_t size);
void __real__free_r(struct _reent *r, void *p);
void *__real__realloc_r(struct _reent *r, void *p, size_t size);
void *__real__calloc_r(struct _reent *r, size_t count, size_t size);
void *__real__memalign_r(struct _reent *r, size_t align, size_t size);

void *__wrap__malloc_r(struct _reent *r, size_t size) {
  if (t_in_newlib)
    return __real__malloc_r(r, size);
  void *p = Alloc(size, true);
  Track(p, size);  // tw2de (2026-10-26)
  return p;
}

void __wrap__free_r(struct _reent *r, void *p) {
  if (t_in_newlib) {
    __real__free_r(r, p);
    return;
  }
  Free(p);
}

void *__wrap__realloc_r(struct _reent *r, void *p, size_t size) {
  if (t_in_newlib)
    return __real__realloc_r(r, p, size);
  if (t_cache)
    MyWaits().heap_libc++;
  return __wrap_realloc(p, size);
}

void *__wrap__calloc_r(struct _reent *r, size_t count, size_t size) {
  if (t_in_newlib)
    return __real__calloc_r(r, count, size);
  if (t_cache)
    MyWaits().heap_libc++;
  return __wrap_calloc(count, size);
}

void *__wrap__memalign_r(struct _reent *r, size_t align, size_t size) {
  if (t_in_newlib)
    return __real__memalign_r(r, align, size);
  // Our blocks are 16-byte aligned: a request for that or less is a malloc.
  void *p;
  if (align <= 16) {
    p = Alloc(size, false);
  } else if (Active() && align <= 4096 && (align & (align - 1)) == 0 && size + align <= kMaxSmall &&
             (p = CacheAlloc(*t_cache, size + align - 16)) != nullptr) {
    // tw2de (2026-11-10): from the thread's slabs too. FF XIII-2's pre-pass reached shaders whose compile made
    // ~25,000 aligned allocations each: in newlib's heap (one lock, fragmented) a shader took 8-12 s instead of 0.2.
    const uintptr_t raw = reinterpret_cast<uintptr_t>(p);
    const uintptr_t q = (raw + align - 1) & ~(uintptr_t(align) - 1);
    if (q != raw) {  // at least 16 bytes up: room for the mark inside the block
      const uint32_t mark = kOffsetMark | uint32_t(q - raw);
      std::memcpy(reinterpret_cast<char *>(q) - kHeader, &mark, sizeof(mark));
      p = reinterpret_cast<void *>(q);
    }
  } else {
    if (t_cache)
      MyWaits().heap_align++;
    if (!Active())
      g_align_inactive.fetch_add(1, std::memory_order_relaxed);
    else if (align > 4096)
      g_align_big.fetch_add(1, std::memory_order_relaxed);
    else if (align & (align - 1))
      g_align_odd.fetch_add(1, std::memory_order_relaxed);
    else
      g_align_large.fetch_add(1, std::memory_order_relaxed);
    if (align > g_align_max.load(std::memory_order_relaxed)) {
      g_align_max.store(align, std::memory_order_relaxed);
      g_align_max_size.store(size, std::memory_order_relaxed);
    }
    HeapTimer timer;
    InNewlib in;
    p = __real__memalign_r(r, align, size);
  }
  Track(p, size);  // tw2de (2026-10-26)
  return p;
}

}  // extern "C"


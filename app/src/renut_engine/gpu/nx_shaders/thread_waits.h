// tw2de (2026-10-21): where a pre-pass thread's time goes besides computing, per thread (the shader pre-pass's
// diagnostics: cores 1 and 2 were at 20-50 % while compiling). Ticks of armGetSystemTick on the console (19.2 MHz),
// steady_clock nanoseconds elsewhere; only differences of the same counter are meaningful.
#pragma once

#include <cstdint>

namespace renut::nxs {

struct ThreadWaits {
  uint64_t nx_lock = 0;       // waiting for our locks (NxMutex / NxSharedMutex) when they were taken
  uint64_t pthread_lock = 0;  // waiting for pthread mutexes when taken (Mesa's tables, libstdc++)
  uint64_t heap = 0;          // inside newlib's malloc / free / realloc (its lock and its work), large blocks
  uint64_t heap_lock = 0;     // of which waiting for newlib's malloc lock (__malloc_lock)
  uint64_t read = 0;          // the store's GLSL read (ShaderStore::ReadGlsl)
  uint64_t store = 0;         // the DKSH cache's add
  uint32_t nx_waits = 0, pthread_waits = 0, heap_calls = 0;
  // What the newlib calls were (thread_alloc.cpp): an allocation outside a ThreadAllocScope, one over 512 KB, a free
  // or realloc of a block newlib made (before the scope, or by newlib itself), newlib's own internal allocations
  // (_malloc_r and co. called by libc: strdup, printf), aligned allocations.
  uint32_t heap_inactive = 0, heap_big = 0, heap_free_foreign = 0, heap_realloc_foreign = 0, heap_libc = 0,
           heap_align = 0;
  uint32_t slab_fail = 0;  // tw2de (2026-11-10): a new slab asked of newlib and not given (the block then from newlib)
};

inline ThreadWaits &MyWaits() {
  static thread_local ThreadWaits w;
  return w;
}

uint64_t WaitTicks();        // now, in the counters' unit
double WaitTicksToMs(uint64_t ticks);

}  // namespace renut::nxs

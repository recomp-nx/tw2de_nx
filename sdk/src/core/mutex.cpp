/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/thread/mutex.h>

#include <thread>

namespace rex::thread {

GlobalCriticalMutex& global_critical_region::mutex() {
  static GlobalCriticalMutex global_mutex;
  return global_mutex;
}

int guest_atomic_lock_mode = 0;

namespace {
// Mode 3 (see GlobalCriticalMutex).
std::atomic<uint32_t> g_regions_active{0};  // threads inside a game atomic region
std::atomic<uint32_t> g_writer{0};          // the global critical region's holder waits for them / keeps them out
thread_local uint32_t t_region_depth = 0;   // this thread's nested regions
thread_local bool t_region_counted = false; // its outermost one is in g_regions_active
thread_local uint32_t t_mutex_depth = 0;    // this thread's recursion on the mutex

inline void Relax() {
#if defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}
}  // namespace

// Waits for the other threads' regions to end. Bounded: a region whose atomic faults (a watched page) takes this
// mutex in the fault handler, so it cannot end while we hold it; the caller then backs off and retries.
bool GlobalCriticalMutex::DrainGuestRegions(bool bounded) {
  g_writer.store(1, std::memory_order_seq_cst);
  const uint32_t own = t_region_counted ? 1 : 0;
  for (uint32_t i = 0; g_regions_active.load(std::memory_order_seq_cst) > own; ++i) {
    if (bounded && i >= 4096) {
      g_writer.store(0, std::memory_order_seq_cst);
      return false;
    }
    Relax();
  }
  return true;
}

void GlobalCriticalMutex::lock() {
  for (;;) {
    m_.lock();
    if (t_mutex_depth++ != 0 || guest_atomic_lock_mode != 3)
      return;
    if (DrainGuestRegions(true))
      return;
    // A region in progress is waiting for this mutex (its fault handler): let it through and try again.
    --t_mutex_depth;
    m_.unlock();
    std::this_thread::yield();
  }
}

bool GlobalCriticalMutex::try_lock() {
  if (!m_.try_lock())
    return false;
  if (t_mutex_depth++ != 0 || guest_atomic_lock_mode != 3)
    return true;
  if (DrainGuestRegions(true))
    return true;
  --t_mutex_depth;
  m_.unlock();
  return false;
}

void GlobalCriticalMutex::unlock() {
  if (--t_mutex_depth == 0 && guest_atomic_lock_mode == 3)
    g_writer.store(0, std::memory_order_seq_cst);
  m_.unlock();
}

void GlobalCriticalMutex::GuestRegionEnter() {
  if (t_region_depth++ != 0 || t_mutex_depth != 0)
    return;
  for (;;) {
    g_regions_active.fetch_add(1, std::memory_order_seq_cst);
    if (g_writer.load(std::memory_order_seq_cst) == 0) {
      t_region_counted = true;
      return;
    }
    g_regions_active.fetch_sub(1, std::memory_order_seq_cst);
    // The holder is in its critical region: wait for it on the mutex itself.
    global_critical_region::mutex().m_.lock();
    global_critical_region::mutex().m_.unlock();
  }
}

void GlobalCriticalMutex::GuestRegionLeave() {
  if (t_region_depth == 0)
    return;
  if (--t_region_depth == 0 && t_region_counted) {
    t_region_counted = false;
    g_regions_active.fetch_sub(1, std::memory_order_seq_cst);
  }
}

std::recursive_mutex& guest_atomic_mutex() {
  static std::recursive_mutex m;
  return m;
}

}  // namespace rex::thread

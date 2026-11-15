// tw2de (2026-10-21): libnx's own locks on the console (Mutex, RwLock: svcArbitrateLock under contention), the
// standard ones elsewhere. Drop-in for std::mutex / std::shared_mutex (std::lock_guard, std::unique_lock and
// std::shared_lock take them).
#pragma once

#include "renut_engine/gpu/nx_shaders/thread_waits.h"

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <mutex>
#include <shared_mutex>
#endif

namespace renut::nxs {

#if defined(__SWITCH__)
class NxMutex {
 public:
  NxMutex() { mutexInit(&m_); }
  NxMutex(const NxMutex &) = delete;
  NxMutex &operator=(const NxMutex &) = delete;
  void lock() {
    if (mutexTryLock(&m_))
      return;
    const uint64_t t0 = armGetSystemTick();  // contended: the wait counted (thread_waits.h)
    mutexLock(&m_);
    ThreadWaits &w = MyWaits();
    w.nx_lock += armGetSystemTick() - t0;
    w.nx_waits++;
  }
  bool try_lock() { return mutexTryLock(&m_); }
  void unlock() { mutexUnlock(&m_); }

 private:
  Mutex m_;
};

class NxSharedMutex {
 public:
  NxSharedMutex() { rwlockInit(&l_); }
  NxSharedMutex(const NxSharedMutex &) = delete;
  NxSharedMutex &operator=(const NxSharedMutex &) = delete;
  void lock() {
    if (rwlockTryWriteLock(&l_))
      return;
    const uint64_t t0 = armGetSystemTick();
    rwlockWriteLock(&l_);
    ThreadWaits &w = MyWaits();
    w.nx_lock += armGetSystemTick() - t0;
    w.nx_waits++;
  }
  bool try_lock() { return rwlockTryWriteLock(&l_); }
  void unlock() { rwlockWriteUnlock(&l_); }
  void lock_shared() {
    if (rwlockTryReadLock(&l_))
      return;
    const uint64_t t0 = armGetSystemTick();
    rwlockReadLock(&l_);
    ThreadWaits &w = MyWaits();
    w.nx_lock += armGetSystemTick() - t0;
    w.nx_waits++;
  }
  bool try_lock_shared() { return rwlockTryReadLock(&l_); }
  void unlock_shared() { rwlockReadUnlock(&l_); }

 private:
  RwLock l_;
};
#else
using NxMutex = std::mutex;
using NxSharedMutex = std::shared_mutex;
#endif

}  // namespace renut::nxs

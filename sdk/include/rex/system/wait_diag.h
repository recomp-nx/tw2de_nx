// tw2de (2026-10-25): where a guest mutant wait and release spend their time (the app's tw2de_diag_sync_waits
// turns it on). Off: one relaxed load of 'on' per NtWaitForSingleObjectEx / NtReleaseMutant.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex::system::wait_diag {

enum Part : uint32_t {
  kWaitLookup,   // NtWaitForSingleObjectEx: the object table lookup
  kWaitBefore,   // XObject::Wait up to the wait itself (CheckTitleTermination, the wait bookkeeping)
  kWaitWait,     // rex::thread::Wait (the mutex and condition variable; blocking when contended)
  kWaitAfter,    // after it to the return (CheckTitleTermination, WaitCallback, the object reference released)
  kRelLookup,    // NtReleaseMutant: the lookup
  kRelRelease,   // XMutant::ReleaseMutant
  kLookupLock,   // inside every object table lookup (both above): taking the global critical region
  kRelMutex,     // inside the release: taking the mutant's own mutex
  kWaitFast,     // the wait part of the waits that took under 20 us (did not block): the uncontended cost
  kParts
};

struct Totals {
  std::atomic<uint64_t> ns[kParts] = {};
  std::atomic<uint64_t> waits{0}, releases{0}, lookups{0}, fast_waits{0};
};

inline std::atomic<bool> on{false};
inline Totals totals;

inline int64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Marks set by XObject::Wait for the entry point that called it (this thread's current wait).
inline thread_local int64_t t_before_wait = 0, t_after_wait = 0;

}  // namespace rex::system::wait_diag

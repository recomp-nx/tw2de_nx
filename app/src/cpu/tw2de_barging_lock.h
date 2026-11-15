// tw2de (2026-11-06): the game mutexes' native lock (tw2de_native_locks.cpp) without the kernel's hand-over, and
// with a guard against starvation (v2, seen32).
//
// Why no hand-over: the libnx mutex unlocks with svcArbitrateUnlock, which gives the lock to a sleeping waiter at once:
// it owns it before it has run again, and every thread that comes by meanwhile waits for that wake-up too. With many
// short holds that queues the threads behind each other (a lock convoy). Run 266f812b, the later burst: 14.8% of the
// main thread's samples in svcArbitrateLock under the game's lock helpers (sites 82404AA0, 8240C548, 82375E80 among
// them), ~7 ms of a 46-52 ms frame; [lockwaits] in play: lock 82BCC678 ~100 waits a frame of ~50 us, held 33-48 us.
//
// Why the guard (v1, seen24, starved the loader): the main thread holds lock 82BCC660 ~34 ms at a time and takes it
// again right after each release; without the hand-over the woken loader thread never won it (waits up to 2.1 s,
// loads several times longer). So a waiter that has waited kStarveNs or more registers itself as the lock's starving
// waiter, and the next release hands the lock straight to it (the word stays held), as the libnx mutex does for every
// waiter. Short waits keep the no-hand-over path; a long one is served as before.
//
// The word: 0 free, 1 held, 2 held and maybe sleepers. 'starver': the starving waiter's id (0 none); 'granted': 1 when
// a release has handed the lock to it. A starving waiter sleeps on 'granted' with a short timeout and also keeps
// trying the word, so a release that ran just before it registered (and saw no starver) costs it at most that timeout.
//
// Wait / Wake / Now are the platform's (Switch: the address arbiter and the system tick; the PC test
// tools/native_tests/barging_lock_test.cpp: futex and steady_clock). The owner and the recursion depth are the caller's.
#pragma once

#include <atomic>
#include <cstdint>

namespace tw2de::barging_lock {

constexpr int64_t kStarveNs = 1'000'000;      // a waiter past 1 ms is handed the lock at the next release
constexpr int64_t kStarverPollNs = 1'000'000;  // a starving waiter's sleep before it tries the word again

struct Word {
  std::atomic<uint32_t> state{0};
  std::atomic<uint32_t> starver{0};
  std::atomic<uint32_t> granted{0};
};

inline bool TryAcquire(Word &w) {
  uint32_t expected = 0;
  return w.state.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
}

// After a failed TryAcquire, by the thread 'self' (not 0). spin(): true while the caller may keep retrying before it
// sleeps. wait(addr, value, timeout_ns): sleep while *addr == value (timeout -1: none; may return early).
// now(): nanoseconds.
template <class Wait, class Spin, class Now>
void AcquireContended(Word &w, uint32_t self, Wait &&wait, Spin &&spin, Now &&now) {
  while (spin()) {
    if (w.state.load(std::memory_order_relaxed) == 0 && TryAcquire(w))
      return;
  }
  const int64_t start = now();
  bool starving = false;
  for (;;) {
    if (starving && w.granted.exchange(0, std::memory_order_acquire) == 1) {
      w.starver.store(0, std::memory_order_relaxed);  // handed over: the word stays held, now by this thread
      return;
    }
    // Marked so that the release wakes a sleeper; taken with 2, since others may still sleep.
    if (w.state.exchange(2, std::memory_order_acquire) == 0) {
      if (starving) {
        // Took it on its own: no longer starving. A release that already chose this thread cannot happen (this thread
        // holds the lock now; only the holder releases).
        w.starver.store(0, std::memory_order_relaxed);
        w.granted.store(0, std::memory_order_relaxed);
      }
      return;
    }
    if (!starving && now() - start >= kStarveNs) {
      uint32_t none = 0;
      starving = w.starver.compare_exchange_strong(none, self, std::memory_order_seq_cst);
    }
    if (starving)
      wait(&w.granted, 0u, kStarverPollNs);
    else
      wait(&w.state, 2u, -1);
  }
}

// wake(addr, count): wake up to count sleepers on addr.
template <class Wake>
void Release(Word &w, Wake &&wake) {
  if (w.starver.load(std::memory_order_seq_cst) != 0) {
    // Handed to the starving waiter: the word stays held (2), so no other thread takes it in between.
    w.state.store(2, std::memory_order_relaxed);
    w.granted.store(1, std::memory_order_release);
    wake(&w.granted, 1);
    return;
  }
  if (w.state.exchange(0, std::memory_order_release) == 2)
    wake(&w.state, 1);
}

}  // namespace tw2de::barging_lock

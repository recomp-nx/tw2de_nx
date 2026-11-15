// tw2de (2026-11-06): two copy threads for the game's large memmoves (tw2de_parallel_memmove.h), the caller
// taking part. The pieces are pulled in turn, so a copy thread that wakes late or not at all only does fewer of them;
// the caller waits only for the pieces a copy thread already took.
//
// Cores 1 and 2 at 0x2C (the render thread's priority, above the game's 0x3B): seen16 had a helper at the game's 0x3B
// lose its core to a game thread of the same priority mid-run (Horizon's 10 ms turns) and hold the run's end. They
// only run while a move is being made; otherwise they sleep on a condition variable. Pieces of about 2 MB.
#include "tw2de_parallel_memmove.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

#if defined(__SWITCH__)
#include <rex/logging.h>

#include "me/me_platform_compat.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"
#else
#include <thread>  // the PC test (tools/native_tests/parallel_memmove_test.cpp)
#endif

namespace tw2de::parallel_memmove {
namespace {

class CopyThreads {
 public:
  // piece(i) for each i < count; the caller does its share.
  void Run(uint32_t count, const std::function<void(uint32_t)> &piece) {
    Start();
    std::atomic<uint32_t> next{0};
    const std::function<void()> work = [&] {
      for (uint32_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < count;)
        piece(i);
    };
    {
      std::lock_guard lock(mutex_);
      work_ = &work;
      busy_ = 0;
      active_ = true;
      ++generation_;
    }
    wake_.notify_all();
    work();
    std::unique_lock lock(mutex_);
    active_ = false;
    done_.wait(lock, [this] { return busy_ == 0; });
    work_ = nullptr;
  }

 private:
  void Start() {
    if (started_)
      return;
    started_ = true;
#if defined(__SWITCH__)
    tw2de::renut_gpu::StartLongLivedThread([] { Instance().Loop(1); }, 64 << 10);
    tw2de::renut_gpu::StartLongLivedThread([] { Instance().Loop(2); }, 64 << 10);
#else
    std::thread([] { Instance().Loop(1); }).detach();
    std::thread([] { Instance().Loop(2); }).detach();
#endif
  }

  void Loop(int core) {
#if defined(__SWITCH__)
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(0x2C);
    const bool core_ok = RexSwitchPinCurrentThreadToCore(core);
    REXLOG_INFO("[memmove] copy thread on core {}{} at priority 0x2C{}", core, core_ok ? "" : " (NOT accepted)",
                priority_ok ? "" : " (NOT accepted)");
#endif
    uint64_t seen = 0;
    for (;;) {
      const std::function<void()> *work;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
        if (!active_)
          continue;  // that run is over
        work = work_;
        ++busy_;
      }
      (*work)();
      {
        std::lock_guard lock(mutex_);
        --busy_;
      }
      done_.notify_all();
    }
  }

 public:
  // Never destroyed: its threads wait on its condition variable until the process ends (a static object's destructor
  // at exit would destroy it under them: the PC test hung at exit that way).
  static CopyThreads &Instance() {
    static CopyThreads *threads = new CopyThreads;
    return *threads;
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  const std::function<void()> *work_ = nullptr;
  uint32_t busy_ = 0;
  uint64_t generation_ = 0;
  bool active_ = false;
  bool started_ = false;  // the caller's thread only (one caller at a time: Large's lock)
};

std::mutex g_caller;  // one large move at a time (the copy threads serve one)

}  // namespace

bool Large(uint8_t *dst, const uint8_t *src, size_t n) {
  if (n < kLargeBytes || dst == src)
    return false;
  const size_t t = dst > src ? size_t(dst - src) : size_t(src - dst);
  // The saving pass (parts * t bytes, overlapping moves only, on the caller alone) kept under an eighth of the move.
  auto fits = [&](uint32_t p) { return Splittable(t, n, p) && (t >= n || size_t(p) * t <= n / 8); };
  uint32_t parts = uint32_t(std::clamp<size_t>(n / kPieceBytes, 3, 32));
  if (!fits(parts))
    parts = 3;
  if (!fits(parts))
    return false;  // the caller's memmove
  std::unique_lock lock(g_caller, std::try_to_lock);
  if (!lock.owns_lock())
    return false;  // another thread's large move is running: this one alone
  thread_local std::vector<uint8_t> saved;
  Move(dst, src, n, parts, saved, [](uint32_t count, auto &&piece) {
    const std::function<void(uint32_t)> fn = [&](uint32_t i) { piece(i); };
    CopyThreads::Instance().Run(count, fn);
  });
  return true;
}

}  // namespace tw2de::parallel_memmove

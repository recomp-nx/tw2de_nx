// tw2de (2026-10-25, tw2de_diag_jobs): a measurement, no change in behaviour. The 2026-10-25 inclusive profile of
// the main thread (walk and fight, 23-26 fps) has it inside the job system's run-and-wait loop (sub_82794E90) for
// nearly all its game-code time: it runs jobs itself while the three CDevCommon Task workers (sub_82795018) are idle
// most of the time (31-37% of a core between the three). This says who runs the jobs and how long they take.
//
// Every job is taken from its queue by sub_82794B68(queue, level, ...): a spinlock at queue+4 (lwarx/stwcx., spun
// without a pause), then the job's node off the level's list (its address in r3; 0 = none). The thread then runs it
// (the main thread through scheduler+32, a worker through scheduler+36) and comes back for the next one. Timed here per
// thread: the calls, the jobs taken, the time inside the take (mostly the spinlock), and each job's run time (from the
// take that returned it to the same thread's next take; a job that runs other jobs inside it ends at the first of
// them). node+8 is a member-function-pointer thunk (sub_8272E340, 82733D90, 82727638, ...: lwz r10,0(r4); lwz r11,
// 4(r4); add r3,r11,r3; bctr) and node+12 the pointer pair it calls through; the job's function is the word node+12
// points at, which the report groups jobs by (with the thunk). Each take names its list: the second argument (r5) is
// the thread's own list (0 the main thread, 1-3 the workers' indices) or -1 (the shared one), so the report says how
// many of a kind's jobs came from a thread's own list (affine: only that thread takes them).
// A [jobs] report every 10 s in the game log.
//
// Off (the default): one cvar read per take. AArch64 (the Switch) only.
#include <rex/cvar.h>
#include <rex/frame_latency.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__SWITCH__)
#include "profiler/switch_cpu_profiler.h"
#endif

REXCVAR_DEFINE_BOOL(tw2de_diag_jobs, false, "tw2de",
                    "Diagnostic: the job system's jobs (sub_82794B68 takes): who runs them (the main thread or the "
                    "CDevCommon Task workers), how many, how long each runs and how long the take's spinlock takes; "
                    "a [jobs] report every 10 s in the game log. No change in behaviour")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__aarch64__)

REX_EXTERN(__imp__sub_82794B68);

namespace {

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr uint32_t kThreads = 16;
struct ThreadRow {
  char name[48] = {};
  std::atomic<uint64_t> takes{0}, jobs{0}, take_ns{0}, run_ns{0}, run_max_ns{0};
};
ThreadRow g_threads[kThreads];
std::atomic<uint32_t> g_thread_count{0};

constexpr uint32_t kKinds = 256;
struct KindRow {
  std::atomic<uint64_t> key{0};  // thunk << 32 | function; 0 = free
  std::atomic<uint64_t> jobs{0}, run_ns{0}, main_jobs{0}, main_ns{0}, affine{0};
};
KindRow g_kinds[kKinds];
std::atomic<uint64_t> g_kinds_overflow{0};

struct ThreadState {
  int32_t index = -1;
  bool main = false;
  int64_t job_start = 0;  // 0: no job taken
  uint64_t job_key = 0;
  bool job_affine = false;
};
thread_local ThreadState t;

ThreadRow &Row() {
  if (t.index < 0) {
    t.index = int32_t(std::min(g_thread_count.fetch_add(1, std::memory_order_relaxed), kThreads - 1));
    ThreadRow &row = g_threads[t.index];
    if (!row.name[0]) {
#if defined(__SWITCH__)
      if (!os::switch_cpu_profiler::TryGetCurrentThreadName(row.name, sizeof(row.name)))
#endif
        std::snprintf(row.name, sizeof(row.name), "thread %d", t.index);
    }
    t.main = std::strncmp(row.name, "Main", 4) == 0;
  }
  return g_threads[t.index];
}

KindRow *Kind(uint64_t key) {
  if (!key)
    return nullptr;
  const uint32_t start = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 56) % kKinds;
  for (uint32_t k = 0; k < kKinds; ++k) {
    KindRow &row = g_kinds[(start + k) % kKinds];
    uint64_t have = row.key.load(std::memory_order_relaxed);
    if (have == 0 && row.key.compare_exchange_strong(have, key, std::memory_order_relaxed))
      have = key;
    if (have == key)
      return &row;
  }
  return nullptr;
}

void Max(std::atomic<uint64_t> &m, uint64_t v) {
  uint64_t cur = m.load(std::memory_order_relaxed);
  while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

std::atomic<int64_t> g_window_start{0};
std::atomic<uint64_t> g_window_swaps{0};
std::atomic<bool> g_reporting{false};

void MaybeReport(int64_t now) {
  int64_t start = g_window_start.load(std::memory_order_relaxed);
  if (start == 0) {
    if (g_window_start.compare_exchange_strong(start, now))
      g_window_swaps.store(rex::latency::SwapCount(), std::memory_order_relaxed);
    return;
  }
  if (now - start < 10'000'000'000ll || g_reporting.exchange(true))
    return;
  const uint64_t swaps = rex::latency::SwapCount();
  const uint64_t frames = std::max<uint64_t>(1, swaps - g_window_swaps.exchange(swaps));
  const double seconds = double(now - start) / 1e9;
  g_window_start.store(now, std::memory_order_relaxed);
  REXLOG_INFO("[jobs] {:.1f} s, {} frames ({:.2f} fps): per frame; take = sub_82794B68 (its spinlock), run = the job",
              seconds, frames, double(frames) / seconds);
  const uint32_t threads = std::min(g_thread_count.load(std::memory_order_relaxed), kThreads);
  for (uint32_t i = 0; i < threads; ++i) {
    ThreadRow &r = g_threads[i];
    const uint64_t takes = r.takes.exchange(0), jobs = r.jobs.exchange(0), take_ns = r.take_ns.exchange(0),
                   run_ns = r.run_ns.exchange(0), run_max = r.run_max_ns.exchange(0);
    if (!takes)
      continue;
    REXLOG_INFO("[jobs]   {:<28} {:7.1f} takes {:6.1f} jobs; take {:6.3f} ms ({:5.2f} us avg); run {:7.3f} ms ({:6.1f} "
                "us avg, longest {:7.1f} us)",
                r.name, double(takes) / frames, double(jobs) / frames, double(take_ns) / 1e6 / frames,
                double(take_ns) / 1e3 / takes, double(run_ns) / 1e6 / frames, jobs ? double(run_ns) / 1e3 / jobs : 0.0,
                double(run_max) / 1e3);
  }
  struct Copy {
    uint64_t key, jobs, ns, main_jobs, main_ns, affine;
  };
  std::vector<Copy> kinds;
  for (KindRow &k : g_kinds) {
    const uint64_t key = k.key.load(std::memory_order_relaxed);
    if (!key)
      continue;
    Copy c{key,
           k.jobs.exchange(0),
           k.run_ns.exchange(0),
           k.main_jobs.exchange(0),
           k.main_ns.exchange(0),
           k.affine.exchange(0)};
    if (c.jobs)
      kinds.push_back(c);
  }
  std::sort(kinds.begin(), kinds.end(), [](const Copy &a, const Copy &b) { return a.ns > b.ns; });
  for (size_t i = 0; i < kinds.size() && i < 30; ++i) {
    const Copy &c = kinds[i];
    REXLOG_INFO("[jobs]     job {:08X} (thunk {:08X}): {:6.1f} jobs {:7.3f} ms ({:6.1f} us avg); by the main thread "
                "{:5.1f} jobs {:6.3f} ms; from a thread's own list {:.0f}%",
                uint32_t(c.key), uint32_t(c.key >> 32), double(c.jobs) / frames, double(c.ns) / 1e6 / frames,
                double(c.ns) / 1e3 / c.jobs, double(c.main_jobs) / frames, double(c.main_ns) / 1e6 / frames,
                100.0 * double(c.affine) / double(c.jobs));
  }
  if (const uint64_t o = g_kinds_overflow.exchange(0))
    REXLOG_INFO("[jobs]     {} jobs of kinds not in the table", o);
  g_reporting.store(false);
}

bool On() {
  static const bool on = REXCVAR_GET(tw2de_diag_jobs);
  return on;
}

}  // namespace

extern "C" REX_FUNC(sub_82794B68) {
  if (!On()) {
    __imp__sub_82794B68(ctx, base);
    return;
  }
  ThreadRow &row = Row();
  const bool own_list = ctx.r5.s32 != -1;
  const int64_t t0 = NowNs();
  // The job this thread took last time has run until now.
  if (t.job_start) {
    const uint64_t ran = uint64_t(t0 - t.job_start);
    row.run_ns.fetch_add(ran, std::memory_order_relaxed);
    Max(row.run_max_ns, ran);
    if (KindRow *k = Kind(t.job_key)) {
      k->jobs.fetch_add(1, std::memory_order_relaxed);
      k->run_ns.fetch_add(ran, std::memory_order_relaxed);
      if (t.job_affine)
        k->affine.fetch_add(1, std::memory_order_relaxed);
      if (t.main) {
        k->main_jobs.fetch_add(1, std::memory_order_relaxed);
        k->main_ns.fetch_add(ran, std::memory_order_relaxed);
      }
    } else {
      g_kinds_overflow.fetch_add(1, std::memory_order_relaxed);
    }
    t.job_start = 0;
  }
  __imp__sub_82794B68(ctx, base);
  const int64_t t1 = NowNs();
  row.takes.fetch_add(1, std::memory_order_relaxed);
  row.take_ns.fetch_add(uint64_t(t1 - t0), std::memory_order_relaxed);
  const uint32_t node = ctx.r3.u32;
  if (node) {
    row.jobs.fetch_add(1, std::memory_order_relaxed);
    const uint32_t w8 = __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + node + 8));
    const uint32_t w12 = __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + node + 12));
    // the function the thunk calls: the first word of the pair node+12 points at (guest memory above 64 KB)
    const uint32_t fn = w12 >= 0x10000 ? __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + w12)) : 0;
    t.job_key = uint64_t(w8) << 32 | fn;
    t.job_affine = own_list;
    t.job_start = t1;
  }
  MaybeReport(t1);
}

#endif  // __aarch64__

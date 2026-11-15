// tw2de (2026-10-25, tw2de_diag_sync_waits): a measurement, no change in behaviour. The 2026-10-25 profile (GPU at
// 2x, the walk and fight at 22-25 fps) has the main thread running game code ~67% of the time and waiting ~23%: at the
// vfx barrier (sub_82E0F880, with the four vfx-qix-effect threads), in the job-completion loop's Sleep(0)
// (sub_829EC680, called by sub_829EBA18 until the job workers are done) and in a game mutex's acquire
// (sub_826277B8: WaitForSingleObject(*obj, INFINITE)). This times every call of those three, per thread and per call
// site (the guest return address), and every 10 s logs the calls a frame and the time a frame they took, so the
// waits can be told apart: many short hand-offs (the wake-up cost of each) or few long ones (the other side's work).
//
// 2026-10-25, second version: the game's lock functions instead of the bare acquire: sub_82628080(lock, tag) (acquire:
// WaitForSingleObject on the mutant handle at lock+0, then lock+4 = tag, lock+8 += 1) and sub_826280E0 (release),
// timed per lock (its guest address and mutant handle, the object's name) and per call site; and the SDK's own parts of
// every mutant wait and release (rex/system/wait_diag.h: object lookup, bookkeeping, the wait, the rest).
//
// 2026-10-25, third version: the vfx threads' side. The main thread waits ~0.9 ms a vfx barrier call while each vfx
// thread runs ~0.33 ms of CPU a call, and a higher priority for the vfx threads (0x3A) changed nothing, so the wait is
// not their waiting for a core. Added: each thread's time between two barrier calls (a vfx thread's round), the
// native spin barrier's waits (sub_8274B028/8274AFC0, tw2de_spin_barrier.cpp: the phases inside a round) and every
// bare mutex acquire sub_826277B8 (also the locks taken without sub_82628080, e.g. the one at 82AB06C0 the vfx
// threads take through sub_82387830), per thread and per lock.
//
// Off (the default): the functions run as before, one cvar read each call. AArch64 (the Switch) only.
// tw2de (2026-11-10): FF XIII-2's functions (timing hooks only: each calls the game's own): the vfx barrier
// sub_82E0F880 (FF XIII 8278F2F0, the same loop), the job yield sub_829EC680 (82795548's call match), the lock class's
// wrappers sub_82628080 / sub_826280E0 (8243E430 / 8243E478, by their place in the class: native_locks.cpp). The
// measurements in the notes are FF XIII's.
#include <rex/cvar.h>
#include <rex/frame_latency.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/wait_diag.h>
#include <rex/system/xobject.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__SWITCH__)
#include "profiler/switch_cpu_profiler.h"
#endif

REXCVAR_DEFINE_BOOL(tw2de_diag_sync_waits, false, "tw2de",
                    "Diagnostic: time the main thread's waits (the vfx barrier sub_82E0F880, the job-completion "
                    "Sleep(0) sub_829EC680, the game's lock acquire sub_82628080 and release sub_826280E0, per lock) "
                    "per thread and call site, and the SDK's parts of each mutant wait and release; a [syncwait] "
                    "report every 10 s in the game log. No change in behaviour")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__aarch64__)

REX_EXTERN(__imp__sub_82E0F880);
REX_EXTERN(__imp__sub_829EC680);
REX_EXTERN(__imp__sub_82628080);
REX_EXTERN(__imp__sub_826280E0);

namespace {

enum Site : uint32_t { kBarrier, kRound, kJobYield, kLock, kUnlock, kBare, kSpin, kSites };
constexpr const char *kSiteName[kSites] = {"vfx barrier 82BE0698",  "between barrier calls",
                                           "job-done Sleep(0) 82886D50", "lock acquire 824D3578",
                                           "lock release 824D35D8", "mutex acquire 826277B8",
                                           "spin barrier wait 8274B028"};

constexpr uint32_t kThreads = 64;  // threads that called any of the three; later ones share the last row
constexpr uint32_t kCallers = 64;  // (site, guest return address, thread) rows

struct Stat {
  std::atomic<uint64_t> calls{0}, ns{0}, max_ns{0};
  void Add(uint64_t t) {
    calls.fetch_add(1, std::memory_order_relaxed);
    ns.fetch_add(t, std::memory_order_relaxed);
    uint64_t m = max_ns.load(std::memory_order_relaxed);
    while (t > m && !max_ns.compare_exchange_weak(m, t, std::memory_order_relaxed)) {
    }
  }
  void Take(uint64_t &c, uint64_t &n, uint64_t &m) {
    c = calls.exchange(0, std::memory_order_relaxed);
    n = ns.exchange(0, std::memory_order_relaxed);
    m = max_ns.exchange(0, std::memory_order_relaxed);
  }
};

struct ThreadRow {
  char name[48] = {};
  Stat site[kSites];
};
ThreadRow g_threads[kThreads];
std::atomic<uint32_t> g_thread_count{0};

struct CallerRow {
  std::atomic<uint64_t> key{0};  // site << 48 | thread << 32 | return address; 0 = free
  Stat stat;
};
CallerRow g_callers[kCallers];

// Per lock (its guest address): acquires and releases from every thread.
constexpr uint32_t kLocks = 48;
struct LockRow {
  std::atomic<uint32_t> addr{0};  // 0 = free
  std::atomic<uint32_t> handle{0};
  Stat acquire, release;
  Stat main_acquire;  // the main thread's (the first thread that reports itself as main below)
  Stat bare;          // sub_826277B8, every caller (includes the acquires above)
};
LockRow g_locks[kLocks];

LockRow *LockFor(uint32_t addr) {
  const uint32_t start = (addr * 2654435761u >> 26) % kLocks;
  for (uint32_t k = 0; k < kLocks; ++k) {
    LockRow &row = g_locks[(start + k) % kLocks];
    uint32_t have = row.addr.load(std::memory_order_relaxed);
    if (have == 0 && row.addr.compare_exchange_strong(have, addr, std::memory_order_relaxed))
      have = addr;
    if (have == addr)
      return &row;
  }
  return nullptr;
}

thread_local int32_t t_index = -1;

uint32_t ThreadIndex() {
  if (t_index < 0) {
    const uint32_t i = std::min(g_thread_count.fetch_add(1, std::memory_order_relaxed), kThreads - 1);
    t_index = int32_t(i);
    ThreadRow &row = g_threads[i];
    if (!row.name[0]) {
#if defined(__SWITCH__)
      if (!os::switch_cpu_profiler::TryGetCurrentThreadName(row.name, sizeof(row.name)))
#endif
        std::snprintf(row.name, sizeof(row.name), "thread %u", i);
    }
  }
  return uint32_t(t_index);
}

void AddCaller(Site site, uint32_t thread, uint32_t lr, uint64_t t) {
  const uint64_t key = uint64_t(site + 1) << 48 | uint64_t(thread) << 32 | lr;
  const uint32_t start = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 58) % kCallers;
  for (uint32_t k = 0; k < kCallers; ++k) {
    CallerRow &row = g_callers[(start + k) % kCallers];
    uint64_t have = row.key.load(std::memory_order_relaxed);
    if (have == 0 && row.key.compare_exchange_strong(have, key, std::memory_order_relaxed))
      have = key;
    if (have == key) {
      row.stat.Add(t);
      return;
    }
  }
  // Table full: counted in the per-thread rows only.
}

std::atomic<int64_t> g_window_start{0};
std::atomic<uint64_t> g_window_swaps{0};
std::atomic<bool> g_reporting{false};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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
  REXLOG_INFO("[syncwait] {:.1f} s, {} frames ({:.2f} fps): calls a frame, ms a frame, average and longest us",
              seconds, frames, double(frames) / seconds);
  const uint32_t threads = std::min(g_thread_count.load(std::memory_order_relaxed), kThreads);
  for (uint32_t i = 0; i < threads; ++i) {
    for (uint32_t s = 0; s < kSites; ++s) {
      uint64_t c, n, m;
      g_threads[i].site[s].Take(c, n, m);
      if (!c)
        continue;
      REXLOG_INFO("[syncwait]   {:<28} {:<28} {:8.1f} calls/frame {:7.3f} ms/frame avg {:8.1f} us max {:8.1f} us",
                  g_threads[i].name, kSiteName[s], double(c) / frames, double(n) / 1e6 / frames,
                  double(n) / 1e3 / c, double(m) / 1e3);
    }
  }
  for (CallerRow &row : g_callers) {
    const uint64_t key = row.key.load(std::memory_order_relaxed);
    if (!key)
      continue;
    uint64_t c, n, m;
    row.stat.Take(c, n, m);
    if (!c || double(n) / 1e6 / frames < 0.05)  // the sites above 0.05 ms a frame
      continue;
    const uint32_t s = uint32_t(key >> 48) - 1, t = uint32_t(key >> 32) & 0xFFFF, lr = uint32_t(key);
    REXLOG_INFO("[syncwait]     from {:08X} ({}, {}): {:.1f} calls/frame {:.3f} ms/frame avg {:.1f} us max {:.1f} us",
                lr, kSiteName[s], t < kThreads ? g_threads[t].name : "?", double(c) / frames,
                double(n) / 1e6 / frames, double(n) / 1e3 / c, double(m) / 1e3);
  }
  for (LockRow &row : g_locks) {
    const uint32_t addr = row.addr.load(std::memory_order_relaxed);
    if (!addr)
      continue;
    uint64_t ac, an, am, rc, rn, rm, mc, mn, mm;
    row.acquire.Take(ac, an, am);
    row.release.Take(rc, rn, rm);
    row.main_acquire.Take(mc, mn, mm);
    uint64_t bc, bn, bm;
    row.bare.Take(bc, bn, bm);
    if (std::max(ac, bc) < frames)  // locks taken at least once a frame
      continue;
    const uint32_t handle = row.handle.load(std::memory_order_relaxed);
    std::string name;
    if (auto *ks = rex::system::kernel_state())
      if (auto obj = ks->object_table()->LookupObject<rex::system::XObject>(handle))
        name = obj->name();
    REXLOG_INFO("[syncwait]   lock {:08X} (mutant {:08X} '{}'): acquire {:.1f}/frame {:.3f} ms/frame avg {:.1f} us max "
                "{:.1f} us; release {:.1f}/frame {:.3f} ms/frame avg {:.1f} us; main thread {:.1f}/frame {:.3f} "
                "ms/frame",
                addr, handle, name, double(ac) / frames, double(an) / 1e6 / frames, double(an) / 1e3 / ac,
                double(am) / 1e3, double(rc) / frames, double(rn) / 1e6 / frames, rc ? double(rn) / 1e3 / rc : 0.0,
                double(mc) / frames, double(mn) / 1e6 / frames);
    REXLOG_INFO("[syncwait]     lock {:08X}: mutex acquire 826277B8 {:.1f}/frame {:.3f} ms/frame avg {:.1f} us max {:.1f} us",
                addr, double(bc) / frames, double(bn) / 1e6 / frames, bc ? double(bn) / 1e3 / bc : 0.0,
                double(bm) / 1e3);
  }
  {
    namespace wd = rex::system::wait_diag;
    const uint64_t w = wd::totals.waits.exchange(0, std::memory_order_relaxed);
    const uint64_t r = wd::totals.releases.exchange(0, std::memory_order_relaxed);
    double part[wd::kParts];
    for (uint32_t i = 0; i < wd::kParts; ++i)
      part[i] = double(wd::totals.ns[i].exchange(0, std::memory_order_relaxed)) / 1e3;
    if (w)
      REXLOG_INFO("[syncwait]   SDK mutant wait (all threads, {:.1f}/frame), average us: lookup {:.2f}, before the wait "
                  "{:.2f}, the wait {:.2f}, after {:.2f}",
                  double(w) / frames, part[wd::kWaitLookup] / w, part[wd::kWaitBefore] / w, part[wd::kWaitWait] / w,
                  part[wd::kWaitAfter] / w);
    if (r)
      REXLOG_INFO("[syncwait]   SDK mutant release ({:.1f}/frame), average us: lookup {:.2f}, release {:.2f}",
                  double(r) / frames, part[wd::kRelLookup] / r, part[wd::kRelRelease] / r);
    const uint64_t fw = wd::totals.fast_waits.exchange(0, std::memory_order_relaxed);
    if (fw)
      REXLOG_INFO("[syncwait]   SDK mutant waits that did not block ({:.1f}/frame, {:.0f}%): the wait {:.2f} us on "
                  "average",
                  double(fw) / frames, w ? 100.0 * double(fw) / double(w) : 0.0, part[wd::kWaitFast] / fw);
    const uint64_t lk = wd::totals.lookups.exchange(0, std::memory_order_relaxed);
    if (lk)
      REXLOG_INFO("[syncwait]   SDK object lookups ({:.1f}/frame, every type): global critical region taken in {:.2f} us "
                  "on average; mutant release: its own mutex taken in {:.2f} us on average",
                  double(lk) / frames, part[wd::kLookupLock] / lk, r ? part[wd::kRelMutex] / r : 0.0);
  }
  g_reporting.store(false);
}

bool On() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(tw2de_diag_sync_waits);
    rex::system::wait_diag::on.store(v, std::memory_order_relaxed);
    return v;
  }();
  return on;
}

thread_local int64_t t_barrier_return = 0;  // when this thread last left sub_82E0F880

template <typename Fn>
uint64_t Timed(Site site, PPCContext &ctx, Fn &&call) {
  const uint32_t lr = uint32_t(ctx.lr);
  const int64_t t0 = NowNs();
  call();
  const int64_t t1 = NowNs();
  const uint32_t thread = ThreadIndex();
  if (site == kBarrier) {
    if (t_barrier_return)
      g_threads[thread].site[kRound].Add(uint64_t(t0 - t_barrier_return));
    t_barrier_return = t1;
  }
  const uint64_t t = uint64_t(t1 - t0);
  g_threads[thread].site[site].Add(t);
  AddCaller(site, thread, lr, t);
  MaybeReport(t1);
  return t;
}

bool IsMainThread() {
  static thread_local int8_t main = -1;
  if (main < 0) {
    const uint32_t i = ThreadIndex();
    main = std::strncmp(g_threads[i].name, "Main", 4) == 0 ? 1 : 0;
  }
  return main == 1;
}

}  // namespace

// tw2de_spin_barrier.cpp: its waits, timed there (it is a native hook, not a guest function to wrap).
bool FnxfXiiiLrSyncDiagOn() {
  return On();
}
void FnxfXiiiLrSyncDiagSpin(uint32_t lr, uint64_t ns) {
  const uint32_t thread = ThreadIndex();
  g_threads[thread].site[kSpin].Add(ns);
  AddCaller(kSpin, thread, lr, ns);
}

// cpu/tw2de_native_locks.cpp (sub_826277B8 is hooked there): one mutex acquire, timed there.
void FnxfXiiiLrSyncDiagBare(uint32_t lock, uint32_t handle, uint32_t lr, uint64_t ns) {
  const uint32_t thread = ThreadIndex();
  g_threads[thread].site[kBare].Add(ns);
  AddCaller(kBare, thread, lr, ns);
  if (LockRow *row = LockFor(lock)) {
    row->handle.store(handle, std::memory_order_relaxed);
    row->bare.Add(ns);
  }
  MaybeReport(NowNs());
}

extern "C" REX_FUNC(sub_82E0F880) {
  if (!On()) {
    __imp__sub_82E0F880(ctx, base);
    return;
  }
  Timed(kBarrier, ctx, [&] { __imp__sub_82E0F880(ctx, base); });
}

extern "C" REX_FUNC(sub_829EC680) {
  if (!On()) {
    __imp__sub_829EC680(ctx, base);
    return;
  }
  Timed(kJobYield, ctx, [&] { __imp__sub_829EC680(ctx, base); });
}

extern "C" REX_FUNC(sub_82628080) {
  if (!On()) {
    __imp__sub_82628080(ctx, base);
    return;
  }
  const uint32_t lock = ctx.r3.u32;
  const uint64_t t = Timed(kLock, ctx, [&] { __imp__sub_82628080(ctx, base); });
  if (LockRow *row = LockFor(lock)) {
    row->handle.store(__builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + lock)), std::memory_order_relaxed);
    row->acquire.Add(t);
    if (IsMainThread())
      row->main_acquire.Add(t);
  }
}

extern "C" REX_FUNC(sub_826280E0) {
  if (!On()) {
    __imp__sub_826280E0(ctx, base);
    return;
  }
  const uint32_t lock = ctx.r3.u32;
  const uint64_t t = Timed(kUnlock, ctx, [&] { __imp__sub_826280E0(ctx, base); });
  if (LockRow *row = LockFor(lock))
    row->release.Add(t);
}

#endif  // __aarch64__

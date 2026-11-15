// tw2de (2026-10-25, tw2de_native_game_locks): the game's own mutex class on a native lock.
//
// The game's engine mutex ("white%08x" mutants) is a 12-byte object: +0 a mutant handle (CreateMutex in its
// constructor sub_82627748, CloseHandle in its destructor sub_826277B0), +4 the owner's tag and +8 a count (both kept
// by the wrapper sub_82628080/824D35D8 around the calls below). The mutant itself is used by three functions only:
//   sub_826277B8(lock)  lock:     WaitForSingleObject(lock->handle, INFINITE)
//   sub_82627708(lock)  try-lock: WaitForSingleObject(lock->handle, 0); 1 when taken, 0 when not (258 / 128)
//   sub_826277C8(lock)  unlock:   ReleaseMutex(lock->handle)
// Checked in the generated code (2026-10-25): ReleaseMutex (sub_82D3E1F0) is called by sub_826277C8 and by the
// sub_82A38xxx group, which creates its own mutants (sub_82A38918/82A38A50); of the 29 callers of WaitForSingleObject
// (sub_82D35630) only these two read the handle at lock+0 of this class (the others wait on events, threads or handles
// passed in, and wait-then-close joins); the destructor only closes it.
//
// Every call goes through the kernel object table (its global lock), the mutant's host mutex and condition variable:
// the 2026-10-25 [syncwait] run at 22-25 fps has the main thread doing ~740 acquires a frame (4.7 ms a frame, 6.3 us
// each, almost all uncontended) and as many releases. Here each lock (by its guest address) gets a libnx mutex instead,
// recursive like a mutant: an uncontended lock or unlock is one atomic operation; a contended one waits in the kernel's
// arbiter (svcArbitrateLock). The guest mutant stays, untouched (always free): the destructor still closes it.
//
// Same exclusion as the mutant, same results to the callers (lock 0 = WAIT_OBJECT_0, try-lock 1/0, unlock 1, or 0 when
// the thread does not own it, like ReleaseMutex). Differences: a thread that exits holding one leaves it held (a
// mutant would be abandoned; the game does not exit threads holding its locks as far as seen), and a lock whose
// handle is 0 (its constructor's CreateMutex failed) keeps the original path.
//
// 2026-11-06 (seen32): back on the lock without the hand-over, now v2 with the starvation guard (a waiter past 1 ms is
// handed the lock at the next release: cpu/tw2de_barging_lock.h; its test reproduces the starved loader and passes).
// seen33: libnx again by default (the numbers above the cvar); the word lock with tw2de_diag_word_game_locks. The seen24 history:
// 2026-11-06 (seen24, REVERTED in seen25): a lock word without the kernel's hand-over (cpu/tw2de_barging_lock.h)
// starved the loader: run 91a2e9a0, lock 82BCC660 site 821FF540 (the loader's) waited 84-86 ms a frame, single waits up
// to 2.1 s (0.35 s at most before), loads several times longer and map sections not ready in time. The main thread
// holds that lock ~34 ms at a time (site 821F6C48) and takes it again right after each release: with the hand-over the
// sleeping loader got it at the release; without it the main thread, running, always won. The libnx mutex (hand-over)
// stays. A version without the hand-over needs a guard against starvation first (a waiter past a time limit handed the
// lock directly).
//
// Tested 2026-10-25 (walk and fight, [syncwait]): the main thread's ~750 acquires a frame from 4.7 ms to 1.7 ms a frame
// (with the diagnostic's own timing in both), releases ~2 us -> ~0.4 us each, no hang; on by default since.
//
// tw2de_diag_lock_contention (2026-10-25, a measurement): with the native locks, which acquires wait for which holder.
// Each lock and call site (the first guest return address above the lock helpers sub_8222C6E0 / 824D3578 / 826277B8,
// from the guest stack's back chain, and the one above it): acquires, the waits (a failed try first) and their time,
// and how long the site holds the lock; each wait is also charged to the site holding the lock when it began. A
// [lockwait] report every 10 s in the game log.
//
// Init-only cvar (a lock must not change sides while held). Switch only; elsewhere the original functions run.
// tw2de (2026-11-10, LR): LR's class, the same calls without FF XIII-2's checks (no assertion calls): try-lock
// sub_82627708, constructor sub_82627748, destructor sub_826277B0 (tail call to CloseHandle), lock sub_826277B8
// (lwz r3,0(r3); li r4,-1; b WaitForSingleObject), unlock sub_826277C8 (tail call to ReleaseMutex); the wrappers
// sub_82628080 (acquire, tag) / sub_826280E0 (release). The addresses below were renamed from FF XIII-2's.
// tw2de (2026-11-10): FF XIII-2's class: lock sub_826277B8, try-lock sub_82627708 (both exact matches of FF XIII's),
// unlock sub_826277C8 (the same instructions, other data addresses), constructor sub_82627748, destructor sub_826277B0,
// ReleaseMutex sub_82D3E1F0. The addresses in the notes above are FF XIII-2's; the measurements are FF XIII's.
#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp
#include <rex/hook.h>
#include <rex/logging.h>

#include <rex/frame_latency.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>

#include "cpu/tw2de_barging_lock.h"
#include "profiler/switch_cpu_profiler.h"
#endif

REXCVAR_DEFINE_BOOL(tw2de_native_game_locks, true, "tw2de",
                    "CPU: the game's own mutex class (sub_826277B8 lock, sub_82627708 try-lock, sub_826277C8 unlock) "
                    "on a native recursive lock per mutex instead of its kernel mutant (object table, host mutex and "
                    "condition variable each call). Same exclusion; the main thread takes ~740 of these a frame in "
                    "battle")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(tw2de_diag_lock_contention, false, "tw2de",
                    "Diagnostic (with tw2de_native_game_locks): the game mutexes' contention by lock and call site: "
                    "waits, their time, the holders the waits were for and how long each site holds its lock; a "
                    "[lockwait] report every 10 s in the game log. No change in behaviour")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tw2de (2026-11-06): what tw2de_diag_lock_contention costs (a guest stack walk and two clock reads on every
// acquire and release) made run 1324a7dd's bursts longer and added drops of its own (gone in af454352 with it off). This
// one does nothing on an uncontended acquire: only a wait is timed and its site read.
REXCVAR_DEFINE_BOOL(tw2de_diag_lock_waits, false, "tw2de",
                    "Diagnostic (with tw2de_native_game_locks): only the game mutex acquires that had to wait, by "
                    "lock and call site, the main thread's apart; nothing added to an acquire that does not wait. A "
                    "[lockwaits] report every 10 s in the game log. No change in behaviour")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tw2de (2026-11-06, seen32): cpu/tw2de_barging_lock.h v2 (no hand-over, a starving waiter handed the lock).
// seen33: back to libnx by default. Run ee0517fb (seen32): the main thread's waits 0.15-5.1 ms a frame (seen23 with
// libnx: 1.2-3.8), all threads' 65-131 ms a frame (34-56), the loader's lock 82BCC660 site 821FF540 65-74 ms a frame
// (2-32), the later burst 2 drops at 43/55 ms (seen29-31: 2-3 at 46-52): no gain for the main thread, more waiting
// elsewhere. Kept as a diagnostic.
REXCVAR_DEFINE_BOOL(tw2de_diag_word_game_locks, false, "tw2de",
                    "Diagnostic (with tw2de_native_game_locks): the game mutexes on the lock without the kernel's "
                    "hand-over (cpu/tw2de_barging_lock.h: a waiter past 1 ms is still handed it), as in seen32, "
                    "instead of libnx mutexes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(tw2de_native_game_locks_spin_us, 10, "tw2de",
                     "CPU (tw2de_native_game_locks): a contended game mutex is retried for up to this many "
                     "microseconds before the thread sleeps in the kernel (0: sleep at once). Most are held 1-8 us, yet "
                     "a wait took 25-70 us: the sleep, the hand-over and the wake cost more than the hold")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_826277B8);
REX_EXTERN(__imp__sub_82627708);
REX_EXTERN(__imp__sub_826277C8);

#if defined(__aarch64__)
// cpu/tw2de_sync_diag.cpp (tw2de_diag_sync_waits): each lock's time, when it is on.
bool FnxfXiiiLrSyncDiagOn();
void FnxfXiiiLrSyncDiagBare(uint32_t lock, uint32_t handle, uint32_t lr, uint64_t ns);
#endif

namespace {

uint32_t MutantHandle(const uint8_t *base, uint32_t lock) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + lock));
}

#if defined(__SWITCH__)

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool DiagOn() {
  static const bool on = REXCVAR_GET(tw2de_diag_lock_contention);
  return on;
}

// ---- tw2de_diag_lock_contention ----
// tw2de (2026-11-10): FF XIII-2's .text (0x82250000 + 0x9E12D0) and its lock class (sub_82627708 ..
// sub_82628140, the same 34 functions in the same order as FF XIII's 0x8243D8C0 .. 0x8243E4D8). FF XIII's other helper
// (sub_8222C6E0) has no match yet: not counted as a helper here (its callers' sites show as the helper itself).
bool InText(uint32_t a) {
  return a >= 0x822C0000u && a < 0x82E8C128u;  // tw2de (2026-11-10): LR's .text (0x822C0000 + 0xBCC128)
}
bool InHelper(uint32_t a) {
  return a >= 0x82627708u && a < 0x82628140u;  // tw2de: LR's class, 38 functions (try-lock .. the constructor wrapper)
}
uint32_t Load32(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
bool StackAddr(uint32_t a) {
  return a >= 0x10000u && a < 0xE0000000u && (a & 3) == 0;
}
// The call site: the first return address above the lock helpers, and the one above it.
struct Site {
  uint32_t site = 0, parent = 0;
};
Site SiteOf(const PPCContext &ctx, const uint8_t *base) {
  uint32_t chain[6] = {uint32_t(ctx.lr)};
  uint32_t n = 1;
  uint32_t sp = ctx.r1.u32;
  while (n < 6 && StackAddr(sp)) {
    const uint32_t prev = Load32(base, sp);
    if (!StackAddr(prev) || prev <= sp)
      break;
    const uint32_t ra = Load32(base, prev - 8);
    if (!InText(ra))
      break;
    chain[n++] = ra;
    sp = prev;
  }
  Site s;
  for (uint32_t i = 0; i < n; ++i) {
    if (!InHelper(chain[i])) {
      s.site = chain[i];
      s.parent = i + 1 < n ? chain[i + 1] : 0;
      break;
    }
  }
  return s;
}

struct SiteRow {
  std::atomic<uint64_t> key{0};  // lock << 32 | site; 0 = free
  std::atomic<uint32_t> parent{0};
  std::atomic<uint64_t> acquires{0}, waits{0}, wait_ns{0}, main_wait_ns{0}, holds{0}, hold_ns{0}, hold_max_ns{0},
      blocked_ns{0}, blocked{0};
};
constexpr uint32_t kSiteRows = 1024;
SiteRow g_sites[kSiteRows];

SiteRow *RowFor(uint32_t lock, uint32_t site) {
  const uint64_t key = uint64_t(lock) << 32 | site;
  if (!key)
    return nullptr;
  const uint32_t start = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 54) % kSiteRows;
  for (uint32_t k = 0; k < kSiteRows; ++k) {
    SiteRow &r = g_sites[(start + k) % kSiteRows];
    uint64_t have = r.key.load(std::memory_order_relaxed);
    if (have == 0 && r.key.compare_exchange_strong(have, key, std::memory_order_relaxed))
      have = key;
    if (have == key)
      return &r;
  }
  return nullptr;
}

thread_local int8_t t_main = -1;
bool IsMain() {
  if (t_main < 0) {
    char name[64] = {};
    // tw2de (2026-11-06): the profiler registers the game's main thread as "main" (profiler.cpp): with only "Main"
    // tested, every report gave the main thread 0 waits (run 085a600a).
    // Decided once the thread has a name (before that: not counted as the main thread, asked again next time).
    if (!os::switch_cpu_profiler::TryGetCurrentThreadName(name, sizeof(name)) || !name[0])
      return false;
    t_main = (std::strcmp(name, "main") == 0 || std::strncmp(name, "Main", 4) == 0) ? 1 : 0;
  }
  return t_main == 1;
}

std::atomic<int64_t> g_window{0};
std::atomic<uint64_t> g_window_swaps{0};
std::atomic<bool> g_reporting{false};

void MaybeReport(int64_t now) {
  int64_t start = g_window.load(std::memory_order_relaxed);
  if (start == 0) {
    if (g_window.compare_exchange_strong(start, now))
      g_window_swaps.store(rex::latency::SwapCount(), std::memory_order_relaxed);
    return;
  }
  if (now - start < 10'000'000'000ll || g_reporting.exchange(true))
    return;
  const uint64_t swaps = rex::latency::SwapCount();
  const double frames = double(std::max<uint64_t>(1, swaps - g_window_swaps.exchange(swaps)));
  g_window.store(now, std::memory_order_relaxed);
  struct Copy {
    uint32_t lock, site, parent;
    uint64_t acquires, waits, wait_ns, main_wait_ns, holds, hold_ns, hold_max, blocked_ns, blocked;
  };
  std::vector<Copy> rows;
  for (SiteRow &r : g_sites) {
    const uint64_t key = r.key.load(std::memory_order_relaxed);
    if (!key)
      continue;
    Copy c{uint32_t(key >> 32),           uint32_t(key),
           r.parent.load(std::memory_order_relaxed),
           r.acquires.exchange(0),        r.waits.exchange(0),
           r.wait_ns.exchange(0),         r.main_wait_ns.exchange(0),
           r.holds.exchange(0),           r.hold_ns.exchange(0),
           r.hold_max_ns.exchange(0),     r.blocked_ns.exchange(0),
           r.blocked.exchange(0)};
    if (c.acquires)
      rows.push_back(c);
  }
  REXLOG_INFO("[lockwait] {:.0f} frames ({:.2f} fps): per frame; site = the guest call site above the lock helpers "
              "(parent: the one above it)",
              frames, frames * 1e9 / double(now - start));
  std::sort(rows.begin(), rows.end(), [](const Copy &a, const Copy &b) { return a.wait_ns > b.wait_ns; });
  for (size_t i = 0; i < rows.size() && i < 20 && rows[i].wait_ns; ++i) {
    const Copy &c = rows[i];
    REXLOG_INFO("[lockwait]   waits: lock {:08X} site {:08X} (parent {:08X}): {:.1f} acquires, {:.1f} waited {:.3f} ms "
                "(main thread {:.3f} ms)",
                c.lock, c.site, c.parent, c.acquires / frames, c.waits / frames, c.wait_ns / 1e6 / frames,
                c.main_wait_ns / 1e6 / frames);
  }
  std::sort(rows.begin(), rows.end(), [](const Copy &a, const Copy &b) { return a.blocked_ns > b.blocked_ns; });
  for (size_t i = 0; i < rows.size() && i < 20 && rows[i].blocked_ns; ++i) {
    const Copy &c = rows[i];
    REXLOG_INFO("[lockwait]   holders waited for: lock {:08X} site {:08X} (parent {:08X}): others waited {:.3f} ms "
                "({:.1f} waits); it holds {:.1f} times, {:.1f} us on average, longest {:.0f} us",
                c.lock, c.site, c.parent, c.blocked_ns / 1e6 / frames, c.blocked / frames, c.holds / frames,
                c.holds ? c.hold_ns / 1e3 / double(c.holds) : 0.0, c.hold_max / 1e3);
  }
  g_reporting.store(false);
}

bool Native() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(tw2de_native_game_locks);
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[locks] the game's mutexes: {}", v ? "native (tw2de_native_game_locks)" : "kernel mutants");
    return v;
  }();
  return on;
}

// One per lock, by its guest address; never freed (a lock built again at the same address reuses its entry, free by
// then like the old one). The game has a few hundred; when the table is full, new locks keep the original path.
struct Entry {
  std::atomic<uint32_t> key{0};  // the lock's guest address; 0 = free
  Mutex mutex = 0;            // libnx: 0 free, else the owner's handle (| the waiters bit) (the default; tw2de_diag_word_game_locks: the word)
  // tw2de (2026-11-06, seen32): the lock without the kernel's hand-over and its owner's thread handle (0: none).
  tw2de::barging_lock::Word word;
  std::atomic<uint32_t> owner{0};
  uint32_t depth = 0;            // the owner's recursion depth (only the owner reads or writes it)
  // tw2de_diag_lock_contention: the outermost acquire's site and time (written by the owner, read by waiters)
  std::atomic<SiteRow *> holder{nullptr};
  int64_t held_since = 0;
};
constexpr uint32_t kEntries = 16384;
Entry g_entries[kEntries];
std::atomic<bool> g_full_logged{false};

Entry *EntryFor(uint32_t lock) {
  const uint32_t start = (lock * 2654435761u) >> 18;  // 14 bits
  for (uint32_t k = 0; k < 64; ++k) {
    Entry &e = g_entries[(start + k) % kEntries];
    uint32_t have = e.key.load(std::memory_order_acquire);
    if (have == 0 && e.key.compare_exchange_strong(have, lock, std::memory_order_acq_rel))
      return &e;
    if (have == lock)
      return &e;
  }
  if (!g_full_logged.exchange(true))
    REXLOG_WARN("[locks] lock table crowded at {:08X}: that lock keeps its kernel mutant", lock);
  return nullptr;
}

// tw2de_native_game_locks_spin_us: retry a held lock for a while before sleeping on it. The kernel hands a released
// mutex straight to a sleeping waiter (svcArbitrateUnlock), which owns it from then on even before it runs again, so a
// waiter whose core is busy keeps everyone else out meanwhile; a spinning one takes it when it is free.
uint64_t SpinTicks() {
  static const uint64_t ticks = [] {
    const int32_t us = std::clamp(REXCVAR_GET(tw2de_native_game_locks_spin_us), 0, 1000);
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[locks] contended game mutexes: retried for up to {} us before sleeping", us);
    return armNsToTicks(uint64_t(us) * 1000);
  }();
  return ticks;
}

bool LibnxLocks() {
  static const bool on = !REXCVAR_GET(tw2de_diag_word_game_locks);
  return on;
}
uint32_t Self() {
  return uint32_t(threadGetCurHandle());
}
// The lock's primitives: the seen32 word, or the libnx mutex (the default; tw2de_diag_word_game_locks: the word).
bool Mine(Entry &e) {
  return LibnxLocks() ? mutexIsLockedByCurrentThread(&e.mutex) : e.owner.load(std::memory_order_relaxed) == Self();
}
bool TryAcquire(Entry &e) {
  return LibnxLocks() ? mutexTryLock(&e.mutex) : tw2de::barging_lock::TryAcquire(e.word);
}
void Acquired(Entry &e) {
  if (!LibnxLocks())
    e.owner.store(Self(), std::memory_order_relaxed);
  e.depth = 1;
}
void Release(Entry &e) {
  if (LibnxLocks()) {
    mutexUnlock(&e.mutex);
    return;
  }
  e.owner.store(0, std::memory_order_relaxed);
  tw2de::barging_lock::Release(e.word, [](std::atomic<uint32_t> *word, int count) {
    svcSignalToAddress(word, SignalType_Signal, 0, count);
  });
}

int64_t TickNs() {
  return int64_t(armTicksToNs(armGetSystemTick()));
}

void LockContended(Entry &e) {
  const uint64_t budget = SpinTicks();
  const uint64_t start = armGetSystemTick();
  auto spin = [&] {
    if (!budget || armGetSystemTick() - start >= budget)
      return false;
    for (int i = 0; i < 16; ++i)
      asm volatile("yield");
    return true;
  };
  if (LibnxLocks()) {
    while (spin()) {
      if (__atomic_load_n(&e.mutex, __ATOMIC_RELAXED) == 0 && mutexTryLock(&e.mutex))
        return;
    }
    mutexLock(&e.mutex);
    return;
  }
  tw2de::barging_lock::AcquireContended(
      e.word, Self(),
      [](std::atomic<uint32_t> *word, uint32_t value, int64_t timeout_ns) {
        svcWaitForAddress(word, ArbitrationType_WaitIfEqual, int64_t(value), timeout_ns);
      },
      spin, TickNs);
}

void Lock(Entry &e) {
  if (Mine(e)) {
    ++e.depth;
    return;
  }
  if (!TryAcquire(e))
    LockContended(e);
  Acquired(e);
}

// ---- tw2de_diag_lock_waits (2026-11-06): the waits only ----
bool WaitsOn() {
  static const bool on = REXCVAR_GET(tw2de_diag_lock_waits);
  return on;
}
struct WaitRow {
  std::atomic<uint64_t> key{0};  // lock << 32 | site; 0 = free
  std::atomic<uint32_t> parent{0};
  std::atomic<uint64_t> waits{0}, wait_ns{0}, main_waits{0}, main_wait_ns{0}, max_ns{0};
};
constexpr uint32_t kWaitRows = 512;
WaitRow g_wait_rows[kWaitRows];
std::atomic<int64_t> g_waits_window{0};
std::atomic<uint64_t> g_waits_window_swaps{0};
std::atomic<bool> g_waits_reporting{false};

WaitRow *WaitRowFor(uint32_t lock, uint32_t site) {
  const uint64_t key = uint64_t(lock) << 32 | site;
  if (!key)
    return nullptr;
  const uint32_t start = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 55) % kWaitRows;
  for (uint32_t k = 0; k < kWaitRows; ++k) {
    WaitRow &r = g_wait_rows[(start + k) % kWaitRows];
    uint64_t have = r.key.load(std::memory_order_relaxed);
    if (have == 0 && r.key.compare_exchange_strong(have, key, std::memory_order_relaxed))
      have = key;
    if (have == key)
      return &r;
  }
  return nullptr;
}

void MaybeReportWaits(int64_t now) {
  int64_t start = g_waits_window.load(std::memory_order_relaxed);
  if (start == 0) {
    if (g_waits_window.compare_exchange_strong(start, now))
      g_waits_window_swaps.store(rex::latency::SwapCount(), std::memory_order_relaxed);
    return;
  }
  if (now - start < 10'000'000'000ll || g_waits_reporting.exchange(true))
    return;
  const uint64_t swaps = rex::latency::SwapCount();
  const double frames = double(std::max<uint64_t>(1, swaps - g_waits_window_swaps.exchange(swaps)));
  g_waits_window.store(now, std::memory_order_relaxed);
  struct Copy {
    uint32_t lock, site, parent;
    uint64_t waits, wait_ns, main_waits, main_wait_ns, max_ns;
  };
  std::vector<Copy> rows;
  uint64_t all_ns = 0, main_ns = 0;
  for (WaitRow &r : g_wait_rows) {
    const uint64_t key = r.key.load(std::memory_order_relaxed);
    if (!key)
      continue;
    Copy c{uint32_t(key >> 32), uint32_t(key), r.parent.load(std::memory_order_relaxed), r.waits.exchange(0),
           r.wait_ns.exchange(0), r.main_waits.exchange(0), r.main_wait_ns.exchange(0), r.max_ns.exchange(0)};
    all_ns += c.wait_ns;
    main_ns += c.main_wait_ns;
    if (c.waits)
      rows.push_back(c);
  }
  REXLOG_INFO("[lockwaits] {:.0f} frames ({:.2f} fps): game mutex waits {:.3f} ms a frame, the main thread's {:.3f} ms; "
              "site = the guest call site above the lock helpers (parent: the one above it)",
              frames, frames * 1e9 / double(now - start), all_ns / 1e6 / frames, main_ns / 1e6 / frames);
  std::sort(rows.begin(), rows.end(), [](const Copy &a, const Copy &b) {
    return a.main_wait_ns != b.main_wait_ns ? a.main_wait_ns > b.main_wait_ns : a.wait_ns > b.wait_ns;
  });
  for (size_t i = 0; i < rows.size() && i < 16; ++i) {
    const Copy &c = rows[i];
    REXLOG_INFO("[lockwaits]   lock {:08X} site {:08X} (parent {:08X}): {:.2f} waits {:.3f} ms a frame, the main "
                "thread {:.2f} waits {:.3f} ms; longest {:.0f} us",
                c.lock, c.site, c.parent, c.waits / frames, c.wait_ns / 1e6 / frames, c.main_waits / frames,
                c.main_wait_ns / 1e6 / frames, c.max_ns / 1e3);
  }
  g_waits_reporting.store(false);
}

// Lock() when its first try failed, with tw2de_diag_lock_waits: the wait timed, by lock and site.
void LockWaitTimed(Entry &e, uint32_t lock, const PPCContext &ctx, const uint8_t *base) {
  const int64_t t0 = Now();
  LockContended(e);
  const int64_t now = Now();
  const uint64_t waited = uint64_t(now - t0);
  const Site s = SiteOf(ctx, base);
  if (WaitRow *row = WaitRowFor(lock, s.site)) {
    row->parent.store(s.parent, std::memory_order_relaxed);
    row->waits.fetch_add(1, std::memory_order_relaxed);
    row->wait_ns.fetch_add(waited, std::memory_order_relaxed);
    if (IsMain()) {
      row->main_waits.fetch_add(1, std::memory_order_relaxed);
      row->main_wait_ns.fetch_add(waited, std::memory_order_relaxed);
    }
    uint64_t m = row->max_ns.load(std::memory_order_relaxed);
    while (waited > m && !row->max_ns.compare_exchange_weak(m, waited, std::memory_order_relaxed)) {
    }
  }
  MaybeReportWaits(now);
}

void LockWithWaits(Entry &e, uint32_t lock, const PPCContext &ctx, const uint8_t *base) {
  if (Mine(e)) {
    ++e.depth;
    return;
  }
  if (!TryAcquire(e))
    LockWaitTimed(e, lock, ctx, base);
  Acquired(e);
}

// Lock() with the measurement (tw2de_diag_lock_contention).
void LockMeasured(Entry &e, uint32_t lock, const PPCContext &ctx, const uint8_t *base) {
  if (Mine(e)) {
    ++e.depth;
    return;
  }
  const Site s = SiteOf(ctx, base);
  SiteRow *row = RowFor(lock, s.site);
  if (row) {
    row->parent.store(s.parent, std::memory_order_relaxed);
    row->acquires.fetch_add(1, std::memory_order_relaxed);
  }
  if (!TryAcquire(e)) {
    SiteRow *holder = e.holder.load(std::memory_order_relaxed);
    const int64_t t0 = Now();
    LockContended(e);
    const uint64_t waited = uint64_t(Now() - t0);
    if (row) {
      row->waits.fetch_add(1, std::memory_order_relaxed);
      row->wait_ns.fetch_add(waited, std::memory_order_relaxed);
      if (IsMain())
        row->main_wait_ns.fetch_add(waited, std::memory_order_relaxed);
    }
    if (holder) {
      holder->blocked.fetch_add(1, std::memory_order_relaxed);
      holder->blocked_ns.fetch_add(waited, std::memory_order_relaxed);
    }
  }
  Acquired(e);
  e.held_since = Now();
  e.holder.store(row, std::memory_order_relaxed);
}

void UnlockMeasured(Entry &e) {
  SiteRow *row = e.holder.load(std::memory_order_relaxed);
  if (row && e.depth == 1 && e.held_since) {
    const int64_t now = Now();
    const uint64_t held = uint64_t(now - e.held_since);
    row->holds.fetch_add(1, std::memory_order_relaxed);
    row->hold_ns.fetch_add(held, std::memory_order_relaxed);
    uint64_t m = row->hold_max_ns.load(std::memory_order_relaxed);
    while (held > m && !row->hold_max_ns.compare_exchange_weak(m, held, std::memory_order_relaxed)) {
    }
    e.held_since = 0;
    e.holder.store(nullptr, std::memory_order_relaxed);
    MaybeReport(now);
  }
}

bool TryLock(Entry &e) {
  if (Mine(e)) {
    ++e.depth;
    return true;
  }
  if (!TryAcquire(e))
    return false;
  Acquired(e);
  return true;
}

bool Unlock(Entry &e) {
  if (!Mine(e))
    return false;  // ReleaseMutex by a thread that does not own it fails
  if (--e.depth == 0)
    Release(e);
  return true;
}

// The entry for this call, or null for the original path. An entry is used for every later call once made.
Entry *Pick(const uint8_t *base, uint32_t lock) {
  if (!Native() || lock < 0x10000 || MutantHandle(base, lock) == 0)
    return nullptr;
  return EntryFor(lock);
}

#endif  // __SWITCH__

#if defined(__aarch64__)
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
#endif

}  // namespace

extern "C" REX_FUNC(sub_826277B8) {
  const uint32_t lock = ctx.r3.u32;
#if defined(__aarch64__)
  const bool diag = FnxfXiiiLrSyncDiagOn();
  const uint32_t lr = uint32_t(ctx.lr);
  const int64_t t0 = diag ? NowNs() : 0;
#endif
#if defined(__SWITCH__)
  if (Entry *e = Pick(base, lock)) {
    if (DiagOn())
      LockMeasured(*e, lock, ctx, base);
    else if (WaitsOn())
      LockWithWaits(*e, lock, ctx, base);
    else
      Lock(*e);
    ctx.r3.u64 = 0;  // WAIT_OBJECT_0
  } else
#endif
  {
    __imp__sub_826277B8(ctx, base);
  }
#if defined(__aarch64__)
  if (diag)
    FnxfXiiiLrSyncDiagBare(lock, MutantHandle(base, lock), lr, uint64_t(NowNs() - t0));
#endif
}

extern "C" REX_FUNC(sub_82627708) {
#if defined(__SWITCH__)
  if (Entry *e = Pick(base, ctx.r3.u32)) {
    ctx.r3.u64 = TryLock(*e) ? 1 : 0;
    return;
  }
#endif
  __imp__sub_82627708(ctx, base);
}

extern "C" REX_FUNC(sub_826277C8) {
#if defined(__SWITCH__)
  if (Entry *e = Pick(base, ctx.r3.u32)) {
    if (DiagOn() && Mine(*e))
      UnlockMeasured(*e);
    ctx.r3.u64 = Unlock(*e) ? 1 : 0;
    return;
  }
#endif
  __imp__sub_826277C8(ctx, base);
}

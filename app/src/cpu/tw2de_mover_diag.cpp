// tw2de (2026-11-06, tw2de_diag_block_mover): a measurement, no change in behaviour.
//
// Run 085a600a / profiler 25a4429d, the later burst (frames 2130-2136, game CPU 51-82 ms): 11.8% of the main thread's
// samples are memcpy under sub_82729978 (the game's memmove: overlap-safe, in memcpy pieces of the distance) called by
// sub_8279A0B8, which sub_8279AB20 runs. sub_8279AB20 is reached only through a vtable (no direct caller): it calls the
// object's vfunc +44, sets obj+84 = 1, runs sub_8279A0B8(obj, obj+80), clears obj+84, calls vfunc +48. sub_8279A0B8
// walks a list of blocks (from obj+72) and moves them through the object's callback at +28 (r3 destination, r4 source,
// r5 size): it looks like a heap's compaction, which is NOT proven. This times every sub_8279AB20 call (thread, time,
// bytes its memmoves copied) and logs every 10 s: calls and ms a frame (the main thread's apart), MB a frame, the
// longest call. Off (the default): one cvar read a call. Switch only.
#include "tw2de_native_crt.h"
#include "tw2de_parallel_memmove.h"

#include <rex/cvar.h>
#include <rex/frame_latency.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#if defined(__SWITCH__)
#include "profiler/switch_cpu_profiler.h"
#endif

REXCVAR_DEFINE_BOOL(tw2de_diag_block_mover, false, "tw2de",
                    "Diagnostic: time the game's block mover sub_8279AB20 (its memmoves under sub_8279A0B8, 11.8% of "
                    "the main thread in a burst): calls, time and bytes a frame, the main thread's apart; a [mover] "
                    "report every 10 s in the game log. No change in behaviour")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, tw2de_native_crt);  // cpu/tw2de_native_crt.cpp

// tw2de (2026-11-06): the per-frame compaction with a byte budget.
//
// sub_8272E4C8, a job run every frame (on the main thread), locks every heap (vfunc +44), calls sub_827B2148 once, then
// each heap's vfunc +28 = sub_8279AB20 (returning to 0x8272E66C), then unlocks them (+48): an incremental
// compaction, three calls a frame. sub_8279AB20 walks the whole block list (sub_8279A0B8, from the first node, no cursor
// kept between calls) and moves every block it can, all in that frame: run 49a8a667, 46.4 MB in one 42.9 ms call at an
// area entry, 26.5 MB / 26.6 ms in a later burst (both on the main thread). The other caller of the walk,
// sub_8279AB88 (the walk and then sub_8279A658: a full compaction, maybe the allocator's), is left as it is.
//
// For the job's calls only (the return address 0x8272E66C), the walk is made in app/CMakeLists.txt from the generated
// sub_8279A0B8 with one test added at its continue point loc_8279A640 (a block done: its bookkeeping, its move
// (+28: sub_827B2200, its memmoves through sub_82729978), +36 (827933E8: blr) and sub_8279AC88): once the call's
// memmoves have reached kBudgetBytes it leaves as at the list's end. The next call (the next frame) starts at the first
// node again and finds the blocks already moved in place. One block is never cut (a block above the budget is moved
// whole, alone).
REXCVAR_DEFINE_BOOL(tw2de_mover_budget, true, "tw2de",
                    "The game's per-frame heap compaction (sub_8279AB20 from its job) stops for the frame once it has "
                    "moved 4 MB, and goes on in the next frame, instead of moving every block in one frame (26-46 MB, "
                    "27-43 ms at an area entry). Off: the game's own walk")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8279AB20);
REX_EXTERN(__imp__sub_8279A0B8);
REX_EXTERN(__imp__sub_82729978);

void FnxfXiiiLrBudgetedWalk(PPCContext &__restrict ctx, uint8_t *base);  // app/CMakeLists.txt (from the generated walk)

namespace {

bool On() {
  static const bool on = REXCVAR_GET(tw2de_diag_block_mover);
  return on;
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool IsMain() {
#if defined(__SWITCH__)
  // Decided once the thread has a name (seen23: decided before the profiler named the main thread, so never "main").
  thread_local int8_t main = -1;
  if (main < 0) {
    char name[64] = {};
    if (!os::switch_cpu_profiler::TryGetCurrentThreadName(name, sizeof(name)) || !name[0])
      return false;
    main = (std::strcmp(name, "main") == 0 || std::strncmp(name, "Main", 4) == 0) ? 1 : 0;
  }
  return main == 1;
#else
  return false;
#endif
}

thread_local uint32_t t_depth = 0;  // inside sub_8279AB20 on this thread

constexpr uint64_t kBudgetBytes = 4u << 20;
constexpr uint32_t kJobReturn = 0x8272E66Cu;  // sub_8272E4C8's call of the heap's vfunc +28
thread_local bool t_budget = false;            // inside a budgeted sub_8279AB20 call
thread_local uint64_t t_budget_bytes = 0;      // its memmoves so far
std::atomic<uint64_t> g_budget_calls{0}, g_budget_cut{0};

bool BudgetOn() {
  static const bool on = REXCVAR_GET(tw2de_mover_budget);
  return on;
}
thread_local uint64_t t_bytes = 0;  // its memmoves' bytes
// tw2de (2026-11-06): the memmoves' own time inside it, and how many of them overlapped (the rest of the call is the
// walk: bookkeeping, virtual calls, and the callback at +36 (r4 = 204 = 0xCC, the old place and the size: a fill?)).
thread_local uint64_t t_move_ns = 0, t_moves = 0, t_overlaps = 0;
std::atomic<uint64_t> g_move_ns{0}, g_moves{0}, g_overlaps{0};
std::atomic<bool> g_pointers_logged{false};

std::atomic<uint64_t> g_calls{0}, g_ns{0}, g_main_calls{0}, g_main_ns{0}, g_bytes{0}, g_max_ns{0}, g_max_bytes{0};
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
  const uint64_t calls = g_calls.exchange(0), ns = g_ns.exchange(0), main_calls = g_main_calls.exchange(0),
                 main_ns = g_main_ns.exchange(0), bytes = g_bytes.exchange(0), max_ns = g_max_ns.exchange(0),
                 max_bytes = g_max_bytes.exchange(0);
  const uint64_t move_ns = g_move_ns.exchange(0), moves = g_moves.exchange(0), overlaps = g_overlaps.exchange(0);
  const uint64_t budget_calls = g_budget_calls.exchange(0), budget_cut = g_budget_cut.exchange(0);
  REXLOG_INFO("[mover] {:.0f} frames: sub_8279AB20 {:.2f} calls {:.3f} ms a frame (the main thread {:.2f} calls "
              "{:.3f} ms), {:.2f} MB a frame moved; the longest call {:.2f} ms, the most bytes in a call {:.2f} MB; "
              "memmoves {} ({} overlapping) {:.1f} ms in all, the rest of the calls {:.1f} ms; budgeted calls {}, "
              "{} of them stopped at the 4 MB budget",
              frames, calls / frames, ns / 1e6 / frames, main_calls / frames, main_ns / 1e6 / frames,
              bytes / 1048576.0 / frames, max_ns / 1e6, max_bytes / 1048576.0, moves, overlaps, move_ns / 1e6,
              (ns > move_ns ? ns - move_ns : 0) / 1e6, budget_calls, budget_cut);
  g_reporting.store(false);
}

void Max(std::atomic<uint64_t> &m, uint64_t v) {
  uint64_t have = m.load(std::memory_order_relaxed);
  while (v > have && !m.compare_exchange_weak(have, v, std::memory_order_relaxed)) {
  }
}

}  // namespace

// The walk's continue point (the generated copy): the budget spent.
bool FnxfXiiiLrMoverBudgetSpent() {
  return t_budget && t_budget_bytes >= kBudgetBytes;
}

extern "C" REX_FUNC(sub_8279A0B8) {
  if (t_budget) {
    FnxfXiiiLrBudgetedWalk(ctx, base);
    if (t_budget_bytes >= kBudgetBytes)
      g_budget_cut.fetch_add(1, std::memory_order_relaxed);  // left before the list's end (or right at it)
    return;
  }
  __imp__sub_8279A0B8(ctx, base);
}

extern "C" REX_FUNC(sub_8279AB20) {
  const bool budget = BudgetOn() && uint32_t(ctx.lr) == kJobReturn && !t_budget;
  if (budget) {
    t_budget = true;
    t_budget_bytes = 0;
    g_budget_calls.fetch_add(1, std::memory_order_relaxed);
  }
  struct Clear {
    bool on;
    ~Clear() {
      if (on)
        t_budget = false;
    }
  } clear{budget};
  if (!On()) {
    __imp__sub_8279AB20(ctx, base);
    return;
  }
  const int64_t t0 = NowNs();
  if (t_depth++ == 0) {
    t_bytes = 0;
    t_move_ns = t_moves = t_overlaps = 0;
  }
  // Once: the object's callbacks (sub_8279A0B8 calls +28 (memmove), +32 (memcpy), +36 (r4 = 0xCC)) and its vtable.
  if (!g_pointers_logged.exchange(true)) {
    const uint32_t obj = ctx.r3.u32;
    auto rd = [&](uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v); };
    REXLOG_INFO("[mover] object {:08X}: vtable {:08X}, +16 {:08X}, +28 {:08X}, +32 {:08X}, +36 {:08X}, +80 {:08X}", obj,
                rd(obj), rd(obj + 16), rd(obj + 28), rd(obj + 32), rd(obj + 36), rd(obj + 80));
  }
  __imp__sub_8279AB20(ctx, base);
  if (--t_depth != 0)
    return;
  const int64_t now = NowNs();
  const uint64_t ns = uint64_t(now - t0);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  g_ns.fetch_add(ns, std::memory_order_relaxed);
  g_bytes.fetch_add(t_bytes, std::memory_order_relaxed);
  g_move_ns.fetch_add(t_move_ns, std::memory_order_relaxed);
  g_moves.fetch_add(t_moves, std::memory_order_relaxed);
  g_overlaps.fetch_add(t_overlaps, std::memory_order_relaxed);
  if (IsMain()) {
    g_main_calls.fetch_add(1, std::memory_order_relaxed);
    g_main_ns.fetch_add(ns, std::memory_order_relaxed);
  }
  Max(g_max_ns, ns);
  Max(g_max_bytes, t_bytes);
  MaybeReport(now);
}

// tw2de (2026-11-06): the game's memmove sub_82729978 (r3 destination, r4 source, r5 size; returns r3), done by the
// host's memmove (tw2de_native_crt). Its own code copies overlapping ranges in memcpy pieces as long as the distance
// between them: the block mover above moves blocks a few bytes down, so 46.4 MB took 41.6 ms in one call (run 3c8ba2bc,
// 1.1 GB/s, in the area entry's window; 26.5 MB in 26 ms in the later burst's), hundreds of thousands of copies. Its three
// paths (no overlap: one copy; destination below the source: pieces from the start; above: pieces from the end) each
// give memmove's result (tools/native_tests/game_memmove_check.cpp: its steps against memmove). Called only through
// pointers (no direct bl in the generated code): no caller can count on its volatile registers. Inside sub_8279AB20 its
// bytes are counted (tw2de_diag_block_mover).
extern "C" REX_FUNC(sub_82729978) {
  if (t_budget)
    t_budget_bytes += ctx.r5.u32;
  int64_t t0 = 0;
  if (t_depth) {
    t_bytes += ctx.r5.u32;
    ++t_moves;
    t_overlaps += tw2de::native_crt::Overlap(ctx.r3.u32, ctx.r5.u32, ctx.r4.u32, ctx.r5.u32) ? 1 : 0;
    t0 = NowNs();
  }
  // tw2de (2026-11-06): a large one over the caller and two copy threads (tw2de_parallel_memmove.h).
  if (REXCVAR_GET(tw2de_native_crt) &&
      tw2de::parallel_memmove::Large(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32)) {
    tw2de::native_crt::Wrote(base, ctx.r3.u32, ctx.r5.u32);
    if (t_depth)
      t_move_ns += uint64_t(NowNs() - t0);
    return;
  }
  if (REXCVAR_GET(tw2de_native_crt) && tw2de::native_crt::CallMemmove(ctx, base)) {
    if (t_depth)
      t_move_ns += uint64_t(NowNs() - t0);
    return;
  }
  __imp__sub_82729978(ctx, base);
  if (t_depth)
    t_move_ns += uint64_t(NowNs() - t0);
}

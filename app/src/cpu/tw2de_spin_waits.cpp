// TW2 (2026-11-12): the game's busy waits made to give the core away.
//
// sub_82FD4BF8(list, value, counters) is the job system's wait for earlier work: list {int *items, count} (sorted
// ascending), it finds the last item <= value (from the end; none, or value below items[0]: no wait) and spins until
// [counters +0x54] <= [counters +0x60] - item (signed compares), re-reading the word. On the Xbox 360 (six hardware
// threads) a spinning thread costs little; here the main thread and three job workers spun in it most of the time
// (rex_profile.log, the prologue cutscene: 42% of the main thread's samples, 88-93% of three workers': about one of the
// Switch's three cores) while the threads that would move the counter waited for a core. The same wait, natively,
// with a yield between reads (std::this_thread::yield: other threads of the same priority on the core run first).
// tw2de_spin_yield off: the game's own loop.
#include <rex/cvar.h>
#include <rex/hook.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

REXCVAR_DEFINE_BOOL(tw2de_spin_yield, true, "tw2de",
                    "The job system's wait for earlier work (sub_82FD4BF8) yields the core between reads instead of "
                    "spinning")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82FD4BF8);

namespace {
int32_t Load(const uint8_t *base, uint32_t va) {
  const uint32_t w = reinterpret_cast<const std::atomic<uint32_t> *>(base + va)->load(std::memory_order_acquire);
  return int32_t(__builtin_bswap32(w));
}
}  // namespace

extern "C" REX_FUNC(sub_82FD4BF8) {
  if (!REXCVAR_GET(tw2de_spin_yield)) {
    __imp__sub_82FD4BF8(ctx, base);
    return;
  }
  const uint32_t list = ctx.r3.u32, counters = ctx.r5.u32;
  const int32_t value = ctx.r4.s32;
  const uint32_t count = uint32_t(Load(base, list + 4));
  if (count <= 1)
    return;
  const uint32_t items = uint32_t(Load(base, list));
  if (value < Load(base, items))
    return;
  for (int32_t i = int32_t(count) - 2; i >= 0; --i) {
    const int32_t item = Load(base, items + uint32_t(i) * 4);
    if (value < item)
      continue;
    const int32_t limit = Load(base, counters + 0x60) - item;
    for (uint32_t spins = 0; Load(base, counters + 0x54) > limit; ++spins)
      if (spins >= 16)
        std::this_thread::yield();
    return;
  }
}

// TW2 (2026-11-12): the main thread's wait while a movie (or another blocking state) plays. sub_824DCDE8 runs, at
// 0x824DD4F8, "while (game->vfunc180()) { sub_82F751C8(); game->[60]->vfunc40(); }": sub_82F751C8 clears a flag
// (lwarx / stwcx.), the second call polls the pads (XamInputGetState for 4 users through sub_82EDF1E8). Nothing in it
// waits: on the console at stock clocks the main thread spent 80-86% of a core there during the intro movie, the core
// the two movie decoder threads needed (the movie and its sound fell behind: audio out found no data 500-1,450 times
// in 10 s). Here the flag call from that loop (return address 0x824DD4FC) first sleeps tw2de_movie_wait_sleep_us: the
// pads are still read every turn, a turn takes that much longer. 0: the game's loop.
REXCVAR_DEFINE_INT32(tw2de_movie_wait_sleep_us, 1000, "tw2de",
                     "The main thread's wait loop while a movie plays sleeps this long a turn (0: spins as the game "
                     "does)")
    .range(0, 16000);

REX_EXTERN(__imp__sub_82F751C8);
extern "C" REX_FUNC(sub_82F751C8) {
  if (uint32_t(ctx.lr) == 0x824DD4FCu) {
    if (const int32_t us = REXCVAR_GET(tw2de_movie_wait_sleep_us); us > 0)
      std::this_thread::sleep_for(std::chrono::microseconds(us));
  }
  __imp__sub_82F751C8(ctx, base);
}

// TW2 (2026-11-12): the job system's "help until the batch is done" loop (sub_82FD5B28: while the batch's count at +84
// is above 0, claim a job with sub_82FD4E70 and run it) spins when every job is claimed but some still run on the
// other threads: claim (an interlocked increment on the batch's shared word), nothing, read the count, claim again.
// On the Switch's three cores the workers that run those jobs share the cores with the main thread (its loop and the
// claim: 7-12 % of it in the prologue). An empty claim from that loop yields the core first. The same claims, the
// same order of everything that happens: only the time between two of them.
REXCVAR_DEFINE_BOOL(tw2de_job_help_yield, true, "tw2de",
                    "TW2: the job system's wait for a batch's last jobs (sub_82FD5B28) yields the core after an empty "
                    "claim instead of spinning on it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82FD4E70);
extern "C" REX_FUNC(sub_82FD4E70) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82FD4E70(ctx, base);
  if (ctx.r3.u32 == 0 && lr == 0x82FD5B58u && REXCVAR_GET(tw2de_job_help_yield))
    std::this_thread::yield();
}

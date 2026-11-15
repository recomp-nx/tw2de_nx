// tw2de (2026-11-10): from FNXF XIII-2 (its sub_8290D670 / sub_82251F28 are LR's sub_82AC0A88 / sub_822EA5F0, the
// same instructions). FF XIII's loading rule around the frame limiter, ported to FF XIII-2 (tw2de_slot_wait_diag.cpp
// holds FF XIII's version, not built here).
//
// The game's resource manager loads in the frame pacer's pre-wait callback (sub_82AC0A88 -> +56), before the main
// thread waits for its vblank, and stops once a vblank has come since the last wait: its limiter sub_822EA5F0 is
// !sub_82AC0AF0() (the pacer's +52, set by the vblank callback its vblank callback). On the 360 a frame's CPU work ends well
// inside the first vblank period of a 30 fps frame, so loading gets the rest of it. Here, when the main thread's frame
// takes longer than 16.7 ms, the vblank has come before the callback runs and the manager stops after one entry a
// frame (FF XIII: 640 entries, 21 s, after a skipped cutscene). So:
//  - while the manager has run less than tw2de_resmgr_min_budget_us this frame, it goes on;
//  - in a frame started at once after a late one (device.cpp StartLateFrameNow), it may load until the vblank the frame
//    would have waited for (frame_interval LateLoadDeadlineUs), as it did inside that wait.
// FF XIII's sub_821F1FE0 / sub_82725728 / sub_82725790; the pacer's layout is the same (+48 event, +52 flag, +56).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>

#include "renut_engine/frame_interval.h"

REXCVAR_DECLARE(bool, log_diagnostics);

REXCVAR_DEFINE_INT32(tw2de_resmgr_min_budget_us, 3000, "Game/Enhancements",
                     "Loading: the game's resource manager may keep loading for at least this many microseconds a "
                     "frame even after the frame's vblank has passed (the game stops at the vblank; when this port's "
                     "frame runs past it, loads crawl at one item a frame). 0 = the game's own rule");

REX_EXTERN(__imp__sub_82AC0A88);
REX_EXTERN(__imp__sub_822EA5F0);

namespace {
std::atomic<uint64_t> g_prewait_t0_us{0};
uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
std::atomic<uint64_t> g_kept{0}, g_late_kept{0};
}  // namespace

// The pacer's pre-wait (the manager's update in its +56 callback, then the wait for the vblank event): when it began.
extern "C" REX_FUNC(sub_82AC0A88) {
  g_prewait_t0_us.store(NowUs(), std::memory_order_relaxed);
  __imp__sub_82AC0A88(ctx, base);
  g_prewait_t0_us.store(0, std::memory_order_relaxed);
}

// The manager's limiter: 1 = no vblank yet, go on.
extern "C" REX_FUNC(sub_822EA5F0) {
  __imp__sub_822EA5F0(ctx, base);
  if (ctx.r3.u32 & 0xFF)
    return;
  const uint64_t now = NowUs();
  if (const uint64_t deadline = tw2de::frame_interval::LateLoadDeadlineUs(); deadline && now < deadline) {
    ctx.r3.u64 = 1;
    const uint64_t n = g_late_kept.fetch_add(1, std::memory_order_relaxed) + 1;
    if (REXCVAR_GET(log_diagnostics) && (n == 1 || n % 5000 == 0))
      REXLOG_INFO("[pacing] the resource manager loads on in a late-started frame ({} times)", n);
    return;
  }
  static const int32_t budget = REXCVAR_GET(tw2de_resmgr_min_budget_us);
  const uint64_t t0 = g_prewait_t0_us.load(std::memory_order_relaxed);
  if (budget > 0 && t0 && now - t0 < uint64_t(budget)) {
    ctx.r3.u64 = 1;
    const uint64_t n = g_kept.fetch_add(1, std::memory_order_relaxed) + 1;
    if (REXCVAR_GET(log_diagnostics) && (n == 1 || n % 5000 == 0))
      REXLOG_INFO("[pacing] the resource manager loads on past the vblank, within its budget ({} times)", n);
  }
}

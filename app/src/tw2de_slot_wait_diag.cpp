// tw2de (2026-10-22, the slow loads after a skipped cutscene): timing of the game's slot waits.
//
// The cutscene thread (Cinema(...)) waits for one of its message queues to drain; that queue's thread was seen stuck
// for up to a minute inside sub_821FF518, the game's "wait for <count> slots" (sub_821F77A8: wait on the slot's
// semaphore while the slot is held). The [waits] report (xobject.cpp) does not see it: it is many short waits, not one
// long one. These wrappers log any call that took long, with the object, the count or slot, the semaphore and the
// caller, so the next slow load says what the queue thread waited for.
#include <fmt/format.h>
#include <rex/cvar.h>
#include "renut_engine/frame_interval.h"  // tw2de (2026-11-05, seen17)
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-24): sdk/src/system/xobject.cpp
// tw2de (2026-10-24): these diagnostics only with log_diagnostics.
#define DIAG_WARN(...) do { if (REXCVAR_GET(log_diagnostics)) REXLOG_WARN(__VA_ARGS__); } while (0)
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>

#ifdef __SWITCH__
#include <switch.h>
extern "C" void RexStallDumpNote(const char* note);  // sdk/src/ui/switch_perf.cpp
#endif

REX_EXTERN(__imp__sub_821FF518);
REX_EXTERN(__imp__sub_821F77A8);
REX_EXTERN(sub_821FF518);
REX_EXTERN(sub_8243E430);
REX_EXTERN(sub_8243E478);
REX_EXTERN(sub_8220DF38);
REX_EXTERN(sub_82208020);
REX_EXTERN(sub_824A4E00);
REX_EXTERN(__imp__sub_827C5EB0);
REX_EXTERN(__imp__sub_827C6E50);
REX_EXTERN(__imp__sub_8275E670);
REX_EXTERN(__imp__sub_82A01370);
REX_EXTERN(__imp__sub_82765C40);
REX_EXTERN(__imp__sub_82A00138);
REX_EXTERN(__imp__sub_82A00838);
REX_EXTERN(__imp__sub_82A00CE8);
REX_EXTERN(__imp__sub_821F1FE0);
REX_EXTERN(__imp__sub_827658A8);
REX_EXTERN(__imp__sub_82765918);

namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t GuestThreadId() {
  auto *t = rex::system::XThread::GetCurrentThread();
  return t ? t->thread_id() : 0;
}

uint32_t LoadBe32(uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}

// tw2de (2026-10-22): while a resource has been waited on for 2 s, a thread polls its two counters (+16 progress,
// +20 pending) every 0.2 ms and, the moment one changes, dumps every thread's stack into rex_stall.log: whoever
// completed the item is caught in (or just past) the code that did it.
std::atomic<uint32_t> g_watch_res{0};
uint8_t *g_watch_base = nullptr;
#ifdef __SWITCH__
Thread g_watch_thread;
// The item the resource is on: its list holds (type, pointer) pairs from +24, and the progress word at +16 counts the
// items done (high half) of the total (low half). Logged with the item's first 128 bytes as hex and text.
void LogCurrentItem(uint32_t r, uint32_t w16, int64_t stuck_ms) {
  const uint32_t index = w16 >> 16;
  const uint32_t type = LoadBe32(g_watch_base, r + 24 + index * 8), ptr = LoadBe32(g_watch_base, r + 24 + index * 8 + 4);
  char hex[3 * 128 + 1] = {}, text[129] = {};
  if (ptr >= 0x40000000u && ptr < 0xA0000000u) {
    for (int i = 0; i < 128; ++i) {
      const uint8_t b = g_watch_base[ptr + i];
      std::snprintf(hex + i * 3, 4, "%02X ", b);
      text[i] = (b >= 0x20 && b < 0x7F) ? char(b) : '.';
    }
  }
  DIAG_WARN("[slots] resource {:08X} on item {} of {} for {} ms: type {:08X} at {:08X}: {}| {}", r, index,
              w16 & 0xFFFF, stuck_ms, type, ptr, hex, text);
}

void WatchMain(void *) {
  uint32_t res = 0, last16 = 0, last20 = 0;
  int64_t since = 0, last_change = 0, next_item_log = 0;
  for (;;) {
    const uint32_t r = g_watch_res.load(std::memory_order_acquire);
    if (!r) {
      res = 0;
      svcSleepThread(50'000'000);
      continue;
    }
    const int64_t now = NowMs();
    const uint32_t w16 = LoadBe32(g_watch_base, r + 16), w20 = LoadBe32(g_watch_base, r + 20);
    if (r != res) {
      res = r;
      last16 = w16;
      last20 = w20;
      since = last_change = now;
      next_item_log = now + 1000;
    } else if (w16 != last16 || w20 != last20) {
      // Only the slow steps (a second or more since the last change): the quick ones are the normal pace.
      if (now - last_change >= 1000) {
        char note[192];
        std::snprintf(note, sizeof(note),
                      "resource %08X changed after %lld ms without (%lld ms into the watch): +16 %08X -> %08X, "
                      "+20 %08X -> %08X", r, (long long)(now - last_change), (long long)(now - since), last16, w16,
                      last20, w20);
        RexStallDumpNote(note);
        DIAG_WARN("[slots] {} (stacks in rex_stall.log)", note);
      }
      last16 = w16;
      last20 = w20;
      last_change = now;
      next_item_log = now + 1000;
    } else if (now >= next_item_log) {
      // Stuck on one item: what it is, and every 5 s what every thread is doing.
      LogCurrentItem(r, w16, now - last_change);
      if (now - last_change >= 4000) {
        char note[128];
        std::snprintf(note, sizeof(note), "resource %08X stuck on item %u for %lld ms", r, w16 >> 16,
                      (long long)(now - last_change));
        RexStallDumpNote(note);
      }
      next_item_log = now + 5000;
    }
    svcSleepThread(200'000);
  }
}
void StartWatch(uint8_t *base, uint32_t res) {
  if (!REXCVAR_GET(log_diagnostics))
    return;  // tw2de (2026-10-24): the watch thread (0x2A, polling) and its stack dumps only for the diagnostics
  static bool started = false;
  g_watch_base = base;
  g_watch_res.store(res, std::memory_order_release);
  if (!started) {
    started = true;
    if (R_SUCCEEDED(threadCreate(&g_watch_thread, WatchMain, nullptr, nullptr, 0x4000, 0x2A, -2)))
      threadStart(&g_watch_thread);
  }
}
#else
void StartWatch(uint8_t *, uint32_t) {}
#endif

}  // namespace

// sub_821FF518(obj, count): waits for count slots of the pool at obj+900.
extern "C" REX_FUNC(sub_821FF518) {
  const uint32_t obj = ctx.r3.u32;
  const int32_t count = ctx.r4.s32;
  const uint32_t caller = uint32_t(ctx.lr);
  const int64_t t0 = NowMs();
  __imp__sub_821FF518(ctx, base);
  const int64_t ms = NowMs() - t0;
  if (ms >= 1000)
    DIAG_WARN("[slots] thread {:X}: {} ms waiting for {} slot(s) of {:08X} (pool {:08X}), called from {:08X}",
                GuestThreadId(), ms, count, obj, obj ? LoadBe32(base, obj + 900) : 0u, caller);
}

// sub_821F77A8(pool, slot): waits on the slot's semaphore while the slot is held.
extern "C" REX_FUNC(sub_821F77A8) {
  const uint32_t pool = ctx.r3.u32;
  const uint32_t slot = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  const int64_t t0 = NowMs();
  __imp__sub_821F77A8(ctx, base);
  const int64_t ms = NowMs() - t0;
  if (ms >= 300)
    DIAG_WARN("[slots] thread {:X}: {} ms on slot {} of pool {:08X} (semaphore {:08X}), called from {:08X}",
                GuestThreadId(), ms, slot, pool,
                slot < 12 ? LoadBe32(base, pool + (slot + 2) * 8 + 4) : 0u, caller);
}

// sub_8220DFB8(resource): the message queue thread's "wait until this resource has nothing pending" (its u16 at +20
// and word at +12 both zero), checked under the resource's lock (*(resource+0)) and, while not, waiting for one slot of
// the job pool at 0x82BEC814 (sub_821FF518(pool, 1): until any job ends). The 047 slow load: no slot wait over 300 ms,
// so the thread polled here for a minute. Same loop natively, with the resource logged every 5 s while it waits.
extern "C" REX_FUNC(sub_8220DFB8) {
  const uint32_t res = ctx.r3.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  sub_82208020(ctx, base);  // the game's own timing of the wait (its debug print below)
  const uint32_t game_t0 = ctx.r3.u32;
  const int64_t t0 = NowMs();
  int64_t next_log = t0 + 5000;
  uint64_t polls = 0;
  for (;;) {
    ctx.r3.u64 = LoadBe32(base, res + 0);
    ctx.r4.u64 = 0;
    sub_8243E430(ctx, base);
    ctx.r3.u64 = res;
    sub_8220DF38(ctx, base);
    const bool idle = ctx.r3.u32 != 0;
    ctx.r3.u64 = LoadBe32(base, res + 0);
    sub_8243E478(ctx, base);
    if (idle)
      break;
    ++polls;
    const int64_t now = NowMs();
    if (now - t0 >= 2000 && g_watch_res.load(std::memory_order_relaxed) != res)
      StartWatch(base, res);
    if (now >= next_log) {
      next_log = now + 5000;
      uint32_t w[16];
      for (int i = 0; i < 16; ++i)
        w[i] = LoadBe32(base, res + i * 4);
      DIAG_WARN("[slots] thread {:X}: {} ms waiting for resource {:08X} to settle ({} polls; +12 = {:08X}, +20 = "
                  "{:04X}), called from {:08X}; words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
                  "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  GuestThreadId(), now - t0, res, polls, w[3], w[5] >> 16, caller, w[0], w[1], w[2], w[3], w[4],
                  w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
    }
    ctx.r3.u64 = LoadBe32(base, 0x82BEC814u);
    ctx.r4.u64 = 1;
    sub_821FF518(ctx, base);
  }
  if (g_watch_res.load(std::memory_order_relaxed) == res)
    g_watch_res.store(0, std::memory_order_release);
  // As the original's tail: its debug print of the time waited (r3, r4 as it leaves them).
  sub_82208020(ctx, base);
  ctx.r4.u64 = uint32_t(ctx.r3.u32 - game_t0);
  ctx.r3.u64 = 0x82004A70u;
  sub_824A4E00(ctx, base);
  const int64_t ms = NowMs() - t0;
  if (ms >= 1000)
    DIAG_WARN("[slots] thread {:X}: resource {:08X} settled after {} ms ({} polls), called from {:08X}",
                GuestThreadId(), res, ms, polls, caller);
}

// tw2de (2026-10-22, v93): the v92 slow load, read again. The resource's current item (its +4/+8 words, not the
// next one in the list) sat in state 3 for 20 s each time: a "leaf" item (type 0x11), whose state 3 is
// sub_827C5EB0(scene): every 'vins' instance of the scene must answer ready (its vtable +80, normally sub_827C6E50,
// which asks the instance's loader at +80, vtable +68). One instance answered "not ready" for ~20 s. These two
// wrappers find which: sub_827C6E50 notes the last instance that said no while inside sub_827C5EB0, and the outer one
// logs that instance (and its loader) once it has blocked for a second, every 5 s after, and when it lets go.
namespace {
thread_local bool t_in_vins_poll = false;
thread_local uint32_t t_vins_blocker = 0;
uint32_t g_blocker = 0;
int64_t g_blocker_since = 0, g_blocker_next_log = 0;
uint32_t g_blocker_calls = 0;

void HexText(uint8_t *base, uint32_t ea, int n, char *hex, char *text) {
  hex[0] = text[0] = 0;
  if (ea < 0x40000000u || ea >= 0xA0000000u)
    return;
  for (int i = 0; i < n; ++i) {
    const uint8_t b = base[ea + i];
    std::snprintf(hex + i * 3, 4, "%02X ", b);
    text[i] = (b >= 0x20 && b < 0x7F) ? char(b) : '.';
  }
  text[n] = 0;
}

void LogBlocker(uint8_t *base, uint32_t inst, int64_t ms, const char *what) {
  const uint32_t vt = LoadBe32(base, inst);
  const uint32_t poll = (vt >= 0x82000000u && vt < 0x84000000u) ? LoadBe32(base, vt + 80) : 0;
  const uint32_t loader = LoadBe32(base, inst + 80);
  const uint32_t lvt = (loader >= 0x40000000u && loader < 0xA0000000u) ? LoadBe32(base, loader) : 0;
  const bool lvt_ok = lvt >= 0x82000000u && lvt < 0x84000000u;
  static char hex[3 * 160 + 1], text[161], lhex[3 * 160 + 1], ltext[161];
  HexText(base, inst, 160, hex, text);
  HexText(base, loader, 160, lhex, ltext);
  DIAG_WARN("[vins] {} {} ms ({} polls): instance {:08X} vtable {:08X} (+80 {:08X}) loader {:08X} vtable {:08X} "
              "(+60 {:08X}, +68 {:08X}); instance: {}| {}; loader: {}| {}",
              what, ms, g_blocker_calls, inst, vt, poll, loader, lvt, lvt_ok ? LoadBe32(base, lvt + 60) : 0u,
              lvt_ok ? LoadBe32(base, lvt + 68) : 0u, hex, text, lhex, ltext);
}
}  // namespace

extern "C" REX_FUNC(sub_827C6E50) {
  const uint32_t inst = ctx.r3.u32;
  __imp__sub_827C6E50(ctx, base);
  if (t_in_vins_poll && !(ctx.r3.u32 & 0xFF) && !t_vins_blocker)
    t_vins_blocker = inst;
}


extern "C" REX_FUNC(sub_827C5EB0) {
  const bool outer = !t_in_vins_poll;
  t_in_vins_poll = true;
  t_vins_blocker = 0;
  __imp__sub_827C5EB0(ctx, base);
  if (!outer)
    return;
  t_in_vins_poll = false;
  const bool ready = (ctx.r3.u32 & 0xFF) != 0;
  const uint32_t blocker = ready ? 0 : (t_vins_blocker ? t_vins_blocker : 1);
  const int64_t now = NowMs();
  if (blocker != g_blocker) {
    if (g_blocker > 1 && now - g_blocker_since >= 250)
      LogBlocker(base, g_blocker, now - g_blocker_since, "let go after");
    g_blocker = blocker;
    g_blocker_since = now;
    g_blocker_next_log = now + 250;
    g_blocker_calls = 0;
  }
  if (!blocker)
    return;
  ++g_blocker_calls;
  if (now >= g_blocker_next_log) {
    g_blocker_next_log = now + 5000;
    if (blocker == 1)
      DIAG_WARN("[vins] scene not ready for {} ms, no instance's sub_827C6E50 said no (another poll function)",
                  now - g_blocker_since);
    else
      LogBlocker(base, blocker, now - g_blocker_since, "not ready for");
  }
}

// tw2de (2026-10-22, v94): v93 named the instance: 's00016' ('vins'), whose loader keeps bit 0x20 of its +100
// flags ("queued in the ExResourceManager") for ~20 s. The manager's update, sub_8275E670 (main thread, every frame),
// walks its queue (node list at +116, the resource 60 bytes before its node) in order and stops at the first resource
// whose update (vtable +108) is not done, so the loader may just be waiting behind another resource. This logs the
// resource at the head of that queue once it has stayed there a second, every 5 s after, and when it leaves.
namespace {
// tw2de (v95): v94 showed the leaf resource 's00016' alone in the queue, its update (sub_82A01A28 ->
// sub_82A01370) walking its entry table ONE entry a frame (+132, the resume index, +150 every 5 s = 150 frames): the
// manager's limiter (sub_8275E510: time since the update began > the limit at +8, in us) says stop after every entry.
// So something in the update takes longer than the limit each frame. Host-time totals of the update's parts, logged
// with the stall lines: what eats the budget.
struct Span {
  uint64_t n = 0, total_us = 0, max_us = 0;
  void Add(uint64_t us) { ++n; total_us += us; if (us > max_us) max_us = us; }
};
Span g_sp_update, g_sp_prelude, g_sp_inner, g_sp_c40, g_sp_e138, g_sp_e838, g_sp_ce8;
uint64_t g_update_t0_us = 0;
uint64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::string SpanText(const char *name, Span &sp) {
  char b[160];
  std::snprintf(b, sizeof(b), " %s %llu x, %llu us total, mean %llu, max %llu;", name, (unsigned long long)sp.n,
                (unsigned long long)sp.total_us, (unsigned long long)(sp.n ? sp.total_us / sp.n : 0),
                (unsigned long long)sp.max_us);
  sp = Span{};
  return b;
}
uint32_t g_rm_head = 0;
int64_t g_rm_since = 0, g_rm_next_log = 0;
uint32_t g_rm_updates = 0;

void LogRmHead(uint8_t *base, uint32_t mgr, uint32_t res, int64_t ms, const char *what) {
  const uint32_t vt = LoadBe32(base, res);
  const bool vt_ok = vt >= 0x82000000u && vt < 0x84000000u;
  static char hex[3 * 192 + 1], text[193];
  HexText(base, res, 192, hex, text);
  // The next few in the queue: resource, vtable and its name (+32), to see what waits behind it.
  std::string rest;
  uint32_t node = LoadBe32(base, res + 60);
  for (int i = 0; i < 6 && node >= 0x40000000u && node < 0xA0000000u; ++i) {
    node = LoadBe32(base, node);
    if (node < 0x40000000u || node >= 0xA0000000u)
      break;
    const uint32_t r = node - 60;
    char name[17] = {};
    for (int k = 0; k < 16; ++k) {
      const uint8_t b = base[r + 32 + k];
      name[k] = (b >= 0x20 && b < 0x7F) ? char(b) : (b ? '.' : 0);
      if (!b)
        break;
    }
    char one[64];
    std::snprintf(one, sizeof(one), " %08X(%08X %s)", r, LoadBe32(base, r), name);
    rest += one;
  }
  const uint64_t limit = (uint64_t(LoadBe32(base, mgr + 8)) << 32) | LoadBe32(base, mgr + 12);
  std::string spans = SpanText("update", g_sp_update) + SpanText("to sub_82A01370", g_sp_prelude) +
                      SpanText("sub_82A01370", g_sp_inner) + SpanText("sub_82765C40", g_sp_c40) +
                      SpanText("sub_82A00138", g_sp_e138) + SpanText("sub_82A00838", g_sp_e838) +
                      SpanText("sub_82A00CE8", g_sp_ce8);
  DIAG_WARN("[resmgr] limit {} us, mode {:02X}, resume index {}; times:{}", limit, base[mgr + 156],
              (LoadBe32(base, res + 132) >> 16), spans);
  DIAG_WARN("[resmgr] {} {} ms ({} updates, queue {}): head {:08X} vtable {:08X} (+108 {:08X}); next:{}; head: {}| {}",
              what, ms, g_rm_updates, LoadBe32(base, mgr + 152) & 0xFFFF, res, vt,
              vt_ok ? LoadBe32(base, vt + 108) : 0u, rest, hex, text);
}
}  // namespace

extern "C" REX_FUNC(sub_8275E670) {
  const uint32_t mgr = ctx.r3.u32;
  g_update_t0_us = NowUs();
  __imp__sub_8275E670(ctx, base);
  g_sp_update.Add(NowUs() - g_update_t0_us);
  g_update_t0_us = 0;
  const uint32_t r3 = ctx.r3.u32;
  const uint32_t node = LoadBe32(base, mgr + 116);
  const uint32_t head = (node >= 0x40000000u && node < 0xA0000000u) ? node - 60 : 0;
  const int64_t now = NowMs();
  if (head != g_rm_head) {
    if (g_rm_head && now - g_rm_since >= 250)
      LogRmHead(base, mgr, g_rm_head, now - g_rm_since, "head left after");
    g_rm_head = head;
    g_rm_since = now;
    g_sp_update = g_sp_prelude = g_sp_inner = g_sp_c40 = g_sp_e138 = g_sp_e838 = g_sp_ce8 = Span{};
    g_rm_next_log = now + 250;
    g_rm_updates = 0;
  }
  ctx.r3.u64 = r3;
  if (!head)
    return;
  ++g_rm_updates;
  if (now >= g_rm_next_log) {
    g_rm_next_log = now + 5000;
    LogRmHead(base, mgr, head, now - g_rm_since, "head unchanged for");
  }
}

#define TW2DE_TIMED(fn, span)                       \
  extern "C" REX_FUNC(fn) {                          \
    const uint64_t t0 = NowUs();                     \
    __imp__##fn(ctx, base);                          \
    span.Add(NowUs() - t0);                          \
  }
TW2DE_TIMED(sub_82765C40, g_sp_c40)
TW2DE_TIMED(sub_82A00138, g_sp_e138)
TW2DE_TIMED(sub_82A00838, g_sp_e838)
TW2DE_TIMED(sub_82A00CE8, g_sp_ce8)
extern "C" REX_FUNC(sub_82A01370) {
  const uint64_t t0 = NowUs();
  if (g_update_t0_us)
    g_sp_prelude.Add(t0 - g_update_t0_us);
  __imp__sub_82A01370(ctx, base);
  g_sp_inner.Add(NowUs() - t0);
}

// tw2de (2026-10-22, v96): the fix. v95: the manager runs in "checker" mode (+156 = 0x10), its limiter asking
// sub_821F1FE0 = !sub_82725790(): "no vblank since the main thread last waited for one" (the frame pacer's +52, set by
// the vblank callback sub_827258D0, cleared by sub_82725728 after its wait). The manager's update runs in that pacer's
// pre-wait callback (sub_82725728 -> +56 -> sub_821F63B8), so it may load until the next vblank. On the 360 the
// frame's CPU work ends well inside the first vblank period of a 30 fps frame and loading gets the rest of it; here,
// when the main thread's frame takes longer than 16.7 ms, the next vblank has already come by the time the callback
// runs, and the manager stops after every entry: one entry a frame, 640 entries of 's00016' = 21 s, twice (the
// "slow load", random as the frame time is, and the character not drawn while its instance waits behind it). Each
// entry costs 5-60 us, so: while the update has run less than this many microseconds, keep going whatever the vblank
// flag says.
REXCVAR_DEFINE_INT32(tw2de_resmgr_min_budget_us, 3000, "Game/Enhancements",
                     "Loading: the game's resource manager may keep loading for at least this many microseconds a "
                     "frame even after the frame's vblank has passed (the game stops at the vblank; when this port's "
                     "frame runs past it, loads crawl at one item a frame, e.g. 40 s after a skipped cutscene). "
                     "0 = the game's own rule");

extern "C" REX_FUNC(sub_821F1FE0) {
  __imp__sub_821F1FE0(ctx, base);
  if (ctx.r3.u32 & 0xFF)
    return;  // no vblank yet: the game keeps loading anyway
  // tw2de (2026-11-05, seen17): a frame started at once after the one before missed its vblank (device.cpp
  // StartLateFrameNow): the vblank it is woken by has come already, but before, it would have waited for the next one
  // and the manager loaded until then. It may still do so: the frame waits only while there is loading to do.
  if (const uint64_t deadline = tw2de::frame_interval::LateLoadDeadlineUs(); deadline && NowUs() < deadline) {
    ctx.r3.u64 = 1;
    return;
  }
  const int32_t budget = REXCVAR_GET(tw2de_resmgr_min_budget_us);
  const uint64_t t0 = g_update_t0_us;
  if (budget > 0 && t0 && NowUs() - t0 < uint64_t(budget))
    ctx.r3.u64 = 1;
}


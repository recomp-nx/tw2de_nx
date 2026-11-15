// tw2de (2026-10-16): two renderers in one NRO. `tw2de_renderer` chooses at boot:
//   "native": tw2de's own native renderer (app/src/me), the default and the exact reference;
//   "renut":  Renut's Direct3D-level renderer (github.com/masterspike52/reNut, imported by tools/renut_gpu_import.py
//             into app/src/renut_engine/gpu), which runs N&B's Direct3D natively through plume: no PM4, no EDRAM tiles.
// Every hook of either renderer is always linked; each runs its renderer's code only when that renderer is chosen and
// the game's original function otherwise. At the addresses both hook (tw2de_shared_hooks.h) tw2de's hook calls
// Renut's side, renut_native_sub_X, when Renut's renderer is chosen.
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc.h>

namespace tw2de::renut_gpu {

// tw2de_renderer == "renut", read once (the choice holds for the session). tw2de (2026-10-19): inline, a byte load
// after the first call (every Direct3D hook asks).
namespace detail {
extern int8_t g_active;  // 1 renut, -1 not, 0 not read yet
bool LatchActive();
}  // namespace detail
inline bool Active() {
  const int8_t a = detail::g_active;
  return a ? a > 0 : detail::LatchActive();
}

// tw2de (2026-10-28, tw2de_diag_hook_time): the time the game's threads spend in Renut's Direct3D hooks, per hook
// and in all (the outermost hook's only), reported every 10 s ([hooktime], HookTimeReport). Off: one byte load and a
// branch a call.
namespace detail {
extern int8_t g_hook_time;  // 1 on, -1 off, 0 not read yet
bool LatchHookTime();
extern thread_local uint32_t t_hook_depth;
}  // namespace detail
inline bool HookTimeOn() {
  const int8_t a = detail::g_hook_time;
  return a ? a > 0 : detail::LatchHookTime();
}
inline uint64_t HookTicks() {
#if defined(__aarch64__)
  uint64_t v;
  asm volatile("mrs %0, cntpct_el0" : "=r"(v));
  return v;
#else
  return 0;
#endif
}
struct HookStat {
  const char *name;
  std::atomic<uint64_t> ticks{0}, calls{0};
  HookStat *next = nullptr;
  explicit HookStat(const char *n);
};
struct HookTimer {
  HookStat *stat = nullptr;
  uint64_t start = 0;
  explicit HookTimer(HookStat &s) {
    if (!HookTimeOn())
      return;
    stat = &s;
    ++detail::t_hook_depth;
    start = HookTicks();
  }
  ~HookTimer() {
    if (!stat)
      return;
    const uint64_t ticks = HookTicks() - start;
    stat->ticks.fetch_add(ticks, std::memory_order_relaxed);
    stat->calls.fetch_add(1, std::memory_order_relaxed);
    if (--detail::t_hook_depth == 0)
      NoteOuterHookTicks(ticks);
  }
  static void NoteOuterHookTicks(uint64_t ticks);
};
// The report, every 10 s (render/draw.cpp calls it each frame; 'frames' is its frame counter).
void HookTimeReport(uint64_t frames);

}  // namespace tw2de::renut_gpu

#define TW2DE_RENUT_HOOK_TIMER(name)                                                   \
  static ::tw2de::renut_gpu::HookStat tw2de_hook_stat_(name);                        \
  const ::tw2de::renut_gpu::HookTimer tw2de_hook_timer_(tw2de_hook_stat_)

// Renut's hook at an address tw2de does not hook.
#define TW2DE_RENUT_HOOK(subroutine, function)                                         \
  REX_EXTERN(__imp__##subroutine);                                                     \
  extern "C" REX_FUNC(subroutine) {                                                    \
    if (::tw2de::renut_gpu::Active()) {                                                \
      TW2DE_RENUT_HOOK_TIMER(#function);                                               \
      ::rex::ppc::HostToGuestFunction<function>(ctx, base);                            \
      return;                                                                          \
    }                                                                                  \
    __imp__##subroutine(ctx, base);                                                    \
  }

// Renut's hook at an address tw2de hooks too: its side only, for tw2de's hook to call.
#define TW2DE_RENUT_HOOK_SHARED(subroutine, function)                                  \
  extern "C" void renut_native_##subroutine(PPCContext& ctx, uint8_t* base) {          \
    TW2DE_RENUT_HOOK_TIMER(#function);                                                 \
    ::rex::ppc::HostToGuestFunction<function>(ctx, base);                              \
  }

// Renut's raw hooks (the body follows the macro, as after REX_HOOK_RAW).
#define TW2DE_RENUT_HOOK_RAW(subroutine)                                               \
  REX_EXTERN(__imp__##subroutine);                                                     \
  static void tw2de_renut_raw_##subroutine(PPCContext& ctx, uint8_t* base);            \
  extern "C" REX_FUNC(subroutine) {                                                    \
    if (::tw2de::renut_gpu::Active()) {                                                \
      TW2DE_RENUT_HOOK_TIMER(#subroutine);                                             \
      tw2de_renut_raw_##subroutine(ctx, base);                                         \
      return;                                                                          \
    }                                                                                  \
    __imp__##subroutine(ctx, base);                                                    \
  }                                                                                    \
  static void tw2de_renut_raw_##subroutine([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)

#define TW2DE_RENUT_HOOK_RAW_SHARED(subroutine)                                        \
  static void tw2de_renut_raw_shared_##subroutine(PPCContext& ctx, uint8_t* base);     \
  extern "C" void renut_native_##subroutine(PPCContext& ctx, uint8_t* base) {           \
    TW2DE_RENUT_HOOK_TIMER(#subroutine);                                               \
    tw2de_renut_raw_shared_##subroutine(ctx, base);                                    \
  }                                                                                    \
  static void tw2de_renut_raw_shared_##subroutine([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)

// For tw2de's own renderer hooks, first thing in their body: with Renut's renderer chosen, Renut's side at a shared
// address (TW2DE_RENUT_YIELD_SHARED) or the game's original (TW2DE_RENUT_YIELD), and nothing of tw2de's renderer.
#define TW2DE_RENUT_YIELD_SHARED(subroutine)                                           \
  if (::tw2de::renut_gpu::Active()) {                                                  \
    renut_native_##subroutine(ctx, base);                                              \
    return;                                                                            \
  }
#define TW2DE_RENUT_YIELD(subroutine)                                                  \
  if (::tw2de::renut_gpu::Active()) {                                                  \
    __imp__##subroutine(ctx, base);                                                    \
    return;                                                                            \
  }

// TW2 (2026-11-14): FMOD Ex's reverb as native code (tw2de_native_reverb.h has what it computes and how it was
// proven). The hook: the native form when it takes the call, the game's own function otherwise.
//
// tw2de_native_reverb off: the game's function, as before. AArch64 (the Switch) only.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>

REXCVAR_DECLARE(bool, log_diagnostics);  // sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(tw2de_native_reverb, true, "Game/Enhancements",
                    "FMOD's reverb (sub_82968720, the largest single function of the game's audio: 11.5% of a core "
                    "in the Tutorial scene at the stock clocks) as native code with the same results, instead of the "
                    "recompiled one")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__aarch64__)
#include "tw2de_native_reverb.h"

REX_EXTERN(__imp__sub_82968720);

extern "C" REX_FUNC(sub_82968720) {
  // The native form never writes FPCR: only where the flush mode is not switched (switch_flush_mode_fixed).
  static const bool on = REXCVAR_GET(tw2de_native_reverb) && rex::ppc::g_flush_mode_fixed;
  if (on) {
    const int32_t channels = ctx.r6.s32;
    const uint32_t frames = ctx.r7.u32;
    if (tw2de::native_reverb::Reverb(ctx, base)) {
      static std::atomic<bool> said{false};
      if (REXCVAR_GET(log_diagnostics) && !said.exchange(true))
        REXLOG_INFO("[native reverb] in use: the first block has {} channels, {} frames", channels, frames);
      return;
    }
    static std::atomic<bool> refused{false};
    if (!refused.exchange(true))
      REXLOG_WARN("[native reverb] a block left to the game's function ({} channels, {} frames): a shape the native "
                  "form does not take (said once)",
                  channels, frames);
  }
  __imp__sub_82968720(ctx, base);
}
#endif

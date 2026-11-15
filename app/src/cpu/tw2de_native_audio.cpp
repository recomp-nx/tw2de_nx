// tw2de (2026-10-24): the game audio mixer's two state-variable filter loops (sub_829C7260, one channel, and
// sub_829C7178, interleaved channels) as native loops (tw2de_native_audio.h). The mixer's thread was 45-69% of a
// core in the 2026-10-24 profile (second battle); these two were a fifth of its samples, with the hottest single code
// line of the thread in the first.
//
// Proven on ARM under qemu-aarch64 against the originals compiled with the Switch compiler and flags (the generated
// code after tools/post_codegen_switch.py, store cards on; the same instructions as the console ELF's, scheduling
// aside): 300,000 trials each, the whole PPCContext, the guest memory window with its store cards and the FPCR the
// same after the call, over ordinary and special inputs (denormals, infinities, quiet and signalling NaNs with
// payloads), 0 to 199 samples (0 to 39 frames of 1 to 8 channels, totals not a whole number of frames), every mode
// and invalid ones, the output over the input, flush to zero on and off before the call. The harness is in
// tools/native_audio_test/.
//
// tw2de_native_audio off: the game's own functions, as before. AArch64 (the Switch) only.
#include "tw2de_native_audio.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(tw2de_native_audio, true, "tw2de",
                    "The game audio mixer's two filter loops as native code instead of the recompiled ones (the same "
                    "results)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__aarch64__)

REX_EXTERN(__imp__sub_829C7260);
REX_EXTERN(__imp__sub_829C7178);

namespace {
std::atomic<bool> g_logged{false};
inline void LogOnce() {
  if (!g_logged.load(std::memory_order_relaxed) && !g_logged.exchange(true))
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[audio] native: the mixer's filter loops (tw2de_native_audio)");
}
}  // namespace

extern "C" REX_FUNC(sub_829C7260) {
  if (!REXCVAR_GET(tw2de_native_audio)) {
    __imp__sub_829C7260(ctx, base);
    return;
  }
  LogOnce();
  tw2de::native_audio::FilterMono(ctx, base);
}

extern "C" REX_FUNC(sub_829C7178) {
  if (!REXCVAR_GET(tw2de_native_audio)) {
    __imp__sub_829C7178(ctx, base);
    return;
  }
  LogOnce();
  tw2de::native_audio::FilterInterleaved(ctx, base);
}

#endif  // __aarch64__

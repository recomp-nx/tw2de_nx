// tw2de (2026-11-10): ported to FF XIII-2 (sub_8228EFC0 / sub_8228F1F0), then to LR: sub_822D09F8 and sub_82395A10 (the same check against FF XIII-2's) compute every stored value as FF XIII's
// sub_82226FB0 / sub_822271E0 do (the same expression trees, operand order included, all loads before the stores, the
// same r9-r11 left; tools: a symbolic evaluation of both games' instructions), only scheduled differently.
// tw2de (2026-10-23): the game's matrix multiply (sub_822D09F8, 615 call sites) and bounding box transform
// (sub_82395A10, 54) as native code (tw2de_native_vmath.h). In the v84 profile the first was the second hottest guest
// function. Proven on ARM, two ways: (1) statically, the Switch objects of the originals and of these compute every
// stored value as the same expression (16 and 28 float operations, the same operand order in each); (2) under
// qemu-aarch64, 300,000 trials each, every output byte, r3..r11 and the FPCR the same over ordinary and special inputs
// (denormals, infinities, NaNs with payloads, several NaNs together, overflow, cancellation, outputs over the inputs,
// unaligned pointers), flush to zero on and off before. The test caught each deliberately wrong variant (one operand
// order flipped, multiply-then-add for the fused one).
// tw2de_native_vmath off: the game's own functions, as before. AArch64 (the Switch) only: elsewhere the originals.
#include "tw2de_native_vmath.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(tw2de_native_vmath, true, "tw2de",
                    "The game's matrix multiply and bounding box transform (its two hottest vector functions) as "
                    "native code instead of the recompiled ones")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__aarch64__)

REX_EXTERN(__imp__sub_822D09F8);

namespace {
std::atomic<bool> g_logged{false};
inline void LogOnce() {
  if (!g_logged.load(std::memory_order_relaxed) && !g_logged.exchange(true))
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[vmath] native: matrix multiply, box transform (tw2de_native_vmath)");
}
}  // namespace
REX_EXTERN(__imp__sub_82395A10);

extern "C" REX_FUNC(sub_822D09F8) {
  if (!REXCVAR_GET(tw2de_native_vmath)) {
    __imp__sub_822D09F8(ctx, base);
    return;
  }
  LogOnce();
  tw2de::native_vmath::MatrixMultiply(ctx, base);
}

extern "C" REX_FUNC(sub_82395A10) {
  if (!REXCVAR_GET(tw2de_native_vmath)) {
    __imp__sub_82395A10(ctx, base);
    return;
  }
  LogOnce();
  tw2de::native_vmath::TransformBox(ctx, base);
}

#endif  // __aarch64__

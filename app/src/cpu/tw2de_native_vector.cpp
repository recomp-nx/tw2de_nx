// tw2de (2026-11-05): sub_825A76F8 (std::vector<unsigned char>::_M_fill_insert) natively, a new buffer through the
// game's own allocator (tw2de_native_vector.h). tw2de_native_vector off: the game's own function, as before.
#include "tw2de_native_vector.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(tw2de_native_vector, true, "tw2de",
                    "Diagnostics: the game's byte-vector fill-insert (sub_825A76F8, the load's hot loop) done with "
                    "memset/memmove on guest memory instead of byte by byte; off: the game's own function")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_825A76F8);
extern "C" REX_FUNC(sub_825A76F8) {
  if (REXCVAR_GET(tw2de_native_vector) &&
      tw2de::native_vector::CallFillInsert(ctx, base, [](uint32_t a) { return rex::runtime::ResolveIndirectFunction(a); }))
    return;
  __imp__sub_825A76F8(ctx, base);
}

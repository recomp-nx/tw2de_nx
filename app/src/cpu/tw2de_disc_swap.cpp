// tw2de (2026-10-31): the disc swaps (disc 1 -> 2 at the end of chapter 4, then 3), with every disc's files in
// the one game folder.
//
// How the game swaps (sub_82440110(disc)):
//   - the disc in is known from its ID file (sub_8243FB80(id, &disc): 32 bytes the discs share, then 3 bytes whose
//     last is the disc's digit, '0' = disc 1), read once at the start; the disc asked for is the one in: done;
//   - else XamSwapDisc(disc, event, texts) through the XDK's wrapper sub_826E0010, the game's "insert disc N" text
//     on the screen and a wait on the event (the SDK sets it at once: xam_info.cpp XamSwapDisc_entry);
//   - then the ID file again: the game goes on when its 32 bytes are the first disc's and its digit the disc asked
//     for, and asks again when not.
// With one folder the ID file is always disc 1's, so the game would ask for ever. Here the disc asked for is noted
// at the swap (sub_826E0010), and every ID read after one (sub_8243FB80) reports that disc: the 32 bytes stay the
// file's, so the check of the game's own files is kept. Before any swap the file's digit is used, as before.
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de: sdk/src/system/xobject.cpp

REX_EXTERN(__imp__sub_826E0010);  // the XDK's XamSwapDisc wrapper: (disc, event handle, texts) -> error
REX_EXTERN(__imp__sub_8243FB80);  // the disc's ID file: (id[32], &disc) -> 0 when read

namespace {
std::atomic<uint32_t> g_asked{0};  // the disc of the last swap (0: none yet)
}  // namespace

extern "C" REX_FUNC(sub_826E0010) {
  const uint32_t disc = ctx.r3.u32;
  if (disc >= 1 && disc <= 3) {
    g_asked.store(disc, std::memory_order_relaxed);
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[disc] the game asks for disc {}", disc);
  }
  __imp__sub_826E0010(ctx, base);
}

extern "C" REX_FUNC(sub_8243FB80) {
  const uint32_t disc_out = ctx.r4.u32;
  __imp__sub_8243FB80(ctx, base);
  const uint32_t asked = g_asked.load(std::memory_order_relaxed);
  if (ctx.r3.u32 != 0 || asked == 0 || disc_out == 0)
    return;
  *reinterpret_cast<uint32_t *>(base + disc_out) = __builtin_bswap32(asked);  // the guest's big-endian word
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[disc] disc {} reported in (every disc is the game folder)", asked);
}

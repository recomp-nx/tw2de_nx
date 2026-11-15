// tw2de (2026-11-01, tw2de_diag_game_log): the game's own debug messages in the game log, for diagnostics.
//
// The game keeps its development messages in the release build: sub_826B9368(format, ...) is its printf-style log
// (232 call sites; the error reporter sub_826B9468(file, line, message), 1225 call sites, formats "file(line): message"
// and passes it on to it). It does nothing unless bit 0x40000000 of the word at 0x82E6FAD8 is set; when set it formats
// the message into a 1 KB buffer (after a prefix "%X:%X> ": a counter and another number) and hands it to sub_8264CB50, its output (one
// caller: this function), which on the console went to a debug channel. Examples the game has: "[SceneDataSheet] push
// command for MoveFloor <%s - %s>", "[Loader] load Navimap id = %08x ...", "Navimap layer id not found".
// With the cvar the bit is set for each call (and put back after it), and the output writes the message to this log
// ("[game] ...") instead of the game's channel. Off (the default): both functions run as before, a cvar read each.
// tw2de (2026-11-10): FF XIII-2's log sub_826B9368 and output sub_8264CB50 (FF XIII's 824A4E00 / 82457D70: the
// same steps, the flag word at 0x82E6FAD4 + 4 tested for bit 0x40000000 the same way, the output its one call), the
// error reporter sub_826B9468; the call counts and examples above are FF XIII's. FF XIII's deny-counter diagnostic
// (its field's load glitch) is not carried over.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

REXCVAR_DEFINE_BOOL(tw2de_diag_game_log, false, "Game/Diagnostics",
                    "Diagnostic: the game's own debug messages (its development log: scene commands, loader, navigation "
                    "map, asserts) into the game log as [game] lines")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_826B9368);  // the game's log: (format, ...)
REX_EXTERN(__imp__sub_8264CB50);  // its output: (message)

namespace {
constexpr uint32_t kLogFlags = 0x83115428;    // bit 0x40000000: the log is on (tw2de 2026-11-10: LR's sub_826B9368, lis/addi 0x83115424, lwz +4; FF XIII-2 0x82E6FAD8)
constexpr uint32_t kLogOn = 0x40000000u;
thread_local int t_inside = 0;
thread_local uint32_t t_caller = 0;  // tw2de (2026-11-09): the log call's return address (who logged)

uint32_t Load32(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
void Store32(uint8_t *base, uint32_t ea, uint32_t v) {
  *reinterpret_cast<uint32_t *>(base + ea) = __builtin_bswap32(v);
}
}  // namespace

extern "C" REX_FUNC(sub_826B9368) {
  if (!REXCVAR_GET(tw2de_diag_game_log)) {
    __imp__sub_826B9368(ctx, base);
    return;
  }
  const uint32_t flags = Load32(base, kLogFlags);
  Store32(base, kLogFlags, flags | kLogOn);
  if (!t_inside)
    t_caller = uint32_t(ctx.lr);
  ++t_inside;
  __imp__sub_826B9368(ctx, base);
  --t_inside;
  Store32(base, kLogFlags, (Load32(base, kLogFlags) & ~kLogOn) | (flags & kLogOn));
}

extern "C" REX_FUNC(sub_8264CB50) {
  if (!REXCVAR_GET(tw2de_diag_game_log) || !t_inside) {
    __imp__sub_8264CB50(ctx, base);
    return;
  }
  const uint32_t msg = ctx.r3.u32;
  if (!msg)
    return;
  const char *s = reinterpret_cast<const char *>(base + msg);
  std::string text(s, strnlen(s, 1024));
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  // The same message many frames running: logged once, then how many times it came.
  static std::mutex m;
  static std::string last;
  static uint32_t repeats = 0;
  std::lock_guard lock(m);
  // The game's prefix ("%X:%X> ": a counter and another number) differs every time: compared without it.
  const size_t cut = text.find("> ");
  const std::string body = cut == std::string::npos ? text : text.substr(cut + 2);
  if (body == last) {
    if (++repeats == 1000)
      REXLOG_INFO("[game] (the last message 1000 more times; no more counted)");
    return;
  }
  if (repeats >= 1 && repeats < 1000)
    REXLOG_INFO("[game] (the last message {} more times)", repeats);
  repeats = 0;
  last = body;
  REXLOG_INFO("[game] {} (from {:08X})", text, t_caller);
}

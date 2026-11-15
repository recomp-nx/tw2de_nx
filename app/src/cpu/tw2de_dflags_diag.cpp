// tw2de (2026-11-01, tw2de_diag_dflags): the game's own debug switches ("dflag", its debug menu's), for diagnostics.
//
// The release build keeps the debug menu's table (0x820506F8 on: name, label, index) and its switches: bits in the
// words at 0x82BF0B44, index i in word i/32, bit 1 << (i % 32) (the menu's handler sub_82465768 and its ON/OFF text,
// sub_8246FFF0, read them so). Nothing but the menu sets them (they are 0 in the image), and the menu cannot be opened.
// Some of them, by the table's names:
//   15 debugdraw   18 player_collision (NoPlayerCollisionCheck)   19 disp_collision (DispCollision)
//   23 disp_occluder   26 disp_aabbtree   28 disp_overlap (DispOverlap)   3 dflag_bg.wire (FloorWire)
//   62 the game's log (with tw2de_diag_game_log: also the messages that test it before logging, sub_821F6E78's)
// (The display ones draw nothing in this release build, Xenia shows the same.)
// The cvar lists the ones to turn on, by index ("19,28,15"); they are set again each frame, in the frame pacer's
// function after its wait (sub_82725728). Empty (the default): that function runs as before, a cvar read more.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <cstdint>
#include <cstdlib>
#include <string>

REXCVAR_DEFINE_STRING(tw2de_diag_dflags, "", "Game/Diagnostics",
                      "Diagnostic: the game's debug switches to turn on, by index (\"19,28,15\": its collision shapes, "
                      "overlap areas, debug drawing)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82725728);  // the frame pacer, after its wait

namespace {
constexpr uint32_t kDflags = 0x82BF0B44;
constexpr int kWords = 3;

struct Wanted {
  uint32_t mask[kWords] = {};
  bool any = false;
};

Wanted Parse() {
  Wanted w;
  const std::string list = REXCVAR_GET(tw2de_diag_dflags);
  const char *p = list.c_str();
  while (*p) {
    char *end = nullptr;
    const long i = std::strtol(p, &end, 0);
    if (end == p) {
      ++p;
      continue;
    }
    p = end;
    if (i < 0 || i >= kWords * 32) {
      REXLOG_WARN("[dflags] {} is not a switch (0..{})", i, kWords * 32 - 1);
      continue;
    }
    w.mask[i >> 5] |= 1u << (i & 31);
    w.any = true;
  }
  if (w.any)
    REXLOG_INFO("[dflags] turned on: {} (words {:08X} {:08X} {:08X})", list, w.mask[0], w.mask[1], w.mask[2]);
  return w;
}
}  // namespace

extern "C" REX_FUNC(sub_82725728) {
  static const Wanted wanted = Parse();
  if (wanted.any) {
    auto *words = reinterpret_cast<uint32_t *>(base + kDflags);
    for (int i = 0; i < kWords; ++i)
      if (wanted.mask[i])
        words[i] = __builtin_bswap32(__builtin_bswap32(words[i]) | wanted.mask[i]);
  }
  __imp__sub_82725728(ctx, base);
}

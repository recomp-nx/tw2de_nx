// TW2 (2026-11-12): the boot menu's language as the game's text and speech language at start-up.
//
// TW2 lists its languages from the files it has (game:\data\<code>0.w2strings / .w2speech; its manager
// at [0x83836A14]: the text languages +0x6C (count +0x70), the speech languages +0x5C (count +0x60), 16-byte strings
// of UTF-16 codes such as "EN"). At start-up (sub_8250DA20) it takes its last languages from the platform and switches
// to them with sub_8250D568(manager, language of list +0x6C, language of list +0x5C); the game's own menu switches the
// same way later (SwitchGameLanguageByIndex, sub_8250D900). Here the first switch takes tw2de_game_language (the boot
// menu's choice: renut_engine/gpu/nx_shaders/boot_menu_switch.cpp) for each list that has it; a list without it
// (a language with text but no speech) keeps the game's choice. Later switches (the game's menu) are the game's.
// The codes are the game's own strings (its table at 0x82019440: PL EN DE IT FR CZ ES ZH RU HU JP, 8 bytes each), set
// with its string assignment sub_82342230(string, wide text).
// TW2 (2026-11-15, Turkish): a code the table lacks (TR: the Xbox 360 game has no Turkish, the PC game has) is
// given as text of its own, written for the calls into a frame made below the stack pointer (the assignment copies
// it). It is set only when the game's list has it, i.e. when the game found a tr0.w2strings; the executable looks for
// "*.w2strings", so a file of that name should be listed (not tried: there is no such file yet).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <string>

REXCVAR_DECLARE(bool, log_diagnostics);  // sdk/src/system/xobject.cpp
REXCVAR_DEFINE_STRING(tw2de_game_language, "", "tw2de",
                      "The game's language at start-up as its code (EN, PL, DE, ...; the boot menu sets it); empty: the "
                      "game's own choice")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8250D568);  // switch languages (manager, text-list string*, speech-list string*)
REX_EXTERN(__imp__sub_82342230);  // string = wide text

namespace {
constexpr uint32_t kCodeTable = 0x82019440;
constexpr const char *kCodes[] = {"PL", "EN", "DE", "IT", "FR", "CZ", "ES", "ZH", "RU", "HU", "JP"};

uint32_t Be32(const uint8_t *p) { return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(p)); }

// The guest's UTF-16 big-endian text at va as ASCII ("" for null).
std::string Code(const uint8_t *base, uint32_t va) {
  std::string s;
  for (uint32_t i = 0; va && i < 8; i++) {
    const uint16_t c = uint16_t(base[va + i * 2] << 8 | base[va + i * 2 + 1]);
    if (!c)
      break;
    s += char(c);
  }
  return s;
}

bool ListHas(const uint8_t *base, uint32_t manager, uint32_t list, const std::string &code) {
  const uint32_t items = Be32(base + manager + list), count = Be32(base + manager + list + 4);
  for (uint32_t i = 0; items && i < count && i < 64; i++)
    if (Code(base, Be32(base + items + i * 16)) == code)
      return true;
  return false;
}
}  // namespace

extern "C" REX_FUNC(sub_8250D568) {
  static std::atomic<bool> first{true};
  const std::string want = REXCVAR_GET(tw2de_game_language);
  if (!first.exchange(false)) {
    __imp__sub_8250D568(ctx, base);
    return;
  }
  uint32_t literal = 0;
  for (uint32_t i = 0; i < sizeof(kCodes) / sizeof(kCodes[0]); i++)
    if (want == kCodes[i])
      literal = kCodeTable + i * 8;
  const uint32_t manager = ctx.r3.u32, text = ctx.r4.u32, speech = ctx.r5.u32;
  const std::string had_text = text ? Code(base, Be32(base + text)) : "", had_speech = speech ? Code(base, Be32(base + speech)) : "";
  bool set_text = false, set_speech = false;
  // A code of two or three letters that is not in the table: its text in a 128-byte frame of this hook's own, above
  // where a callee keeps its link register and homes its arguments.
  const uint64_t stack = ctx.r1.u64;
  if (!literal && want.size() >= 2 && want.size() <= 3) {
    ctx.r1.u64 = stack - 128;
    literal = ctx.r1.u32 + 80;
    for (size_t i = 0; i <= want.size(); i++) {
      base[literal + i * 2] = 0;
      base[literal + i * 2 + 1] = i < want.size() ? uint8_t(want[i]) : 0;
    }
  }
  if (literal && Code(base, literal) == want) {
    auto assign = [&](uint32_t str) {
      const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64;
      ctx.r3.u64 = str;
      ctx.r4.u64 = literal;
      __imp__sub_82342230(ctx, base);
      ctx.r3.u64 = r3;
      ctx.r4.u64 = r4;
      ctx.r5.u64 = r5;
    };
    if (text && ListHas(base, manager, 0x6C, want)) {
      assign(text);
      set_text = true;
    }
    if (speech && ListHas(base, manager, 0x5C, want)) {
      assign(speech);
      set_speech = true;
    }
  }
  ctx.r1.u64 = stack;
  if (REXCVAR_GET(log_diagnostics) || (!want.empty() && !set_text))
    REXLOG_INFO("[language] start-up: the game's {} / {} (text / speech); tw2de_game_language '{}': text {}, speech {}",
                had_text, had_speech, want, set_text ? "set" : "kept", set_speech ? "set" : "kept");
  __imp__sub_8250D568(ctx, base);
}

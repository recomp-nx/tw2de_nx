// tw2de (2026-10-25): the language of the port's own screens (the boot menu, the shader pre-pass, the in-game
// "Compiling shaders..." notice): 0 English, 1 German, 2 French, 3 Spanish, 4 Italian, the boot menu's order.
// TW2 (2026-11-14): 5 Polish, 6 Czech, 7 Hungarian, 8 Russian, 9 Japanese, 10 Chinese (Simplified; drawn with the
// system's Chinese fonts: UseSystemFallbackFonts) (boot_menu_switch.cpp's kLanguages). The
// language comes from the game's language code the boot menu sets (tw2de_game_language: the Xbox language is English
// for Czech and Hungarian); with none set, from the Xbox language as before.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <switch.h>

#include <rex/cvar.h>
#include <rex/system/flags.h>  // user_language

#include "renut_engine/gpu/nx_shaders/prepass_screen.h"  // SetFallbackFonts

REXCVAR_DECLARE(std::string, tw2de_game_language);  // cpu/tw2de_language.cpp

namespace renut::nxs {

constexpr int kUiLanguageCount = 12;  // TW2 (2026-11-15): 11 Turkish

inline int UiLanguage() {
  const std::string code = REXCVAR_GET(tw2de_game_language);
  static constexpr struct {
    const char *code;
    int index;
  } kByCode[] = {{"EN", 0}, {"DE", 1}, {"FR", 2}, {"ES", 3}, {"IT", 4}, {"PL", 5},
                 {"CZ", 6}, {"HU", 7}, {"RU", 8}, {"JP", 9}, {"ZH", 10}, {"TR", 11}};
  for (const auto &c : kByCode)
    if (code == c.code)
      return c.index;
  // No code (the game's own choice): the Xbox 360 language the boot menu sets (1, 3, 4, 5, 6); any other: English.
  switch (REXCVAR_GET(user_language)) {
  case 3: return 1;
  case 4: return 2;
  case 5: return 3;
  case 6: return 4;
  case 14: return 11;  // TW2 (2026-11-15): Turkish
  default: return 0;
  }
}

// The console's system language, for a screen shown before the settings are read (the applet-mode screen, before
// main); any language the port has no text for: English. By the language code's first letters ("fr-CA", "es-419",
// and codes libnx's list does not have).
inline int SystemUiLanguage() {
  int index = 0;
  if (R_FAILED(setInitialize()))
    return 0;
  u64 code = 0;
  if (R_SUCCEEDED(setGetSystemLanguage(&code))) {
    char tag[9] = {};
    std::memcpy(tag, &code, 8);
    static constexpr struct {
      const char *code;
      int index;
    } kByTag[] = {{"de", 1}, {"fr", 2}, {"es", 3}, {"it", 4}, {"pl", 5}, {"cs", 6}, {"hu", 7}, {"ru", 8}, {"ja", 9}, {"zh", 10}, {"tr", 11}};
    for (const auto &c : kByTag)
      if (tag[0] == c.code[0] && tag[1] == c.code[1] && (tag[2] == 0 || tag[2] == '-'))
        index = c.index;
  }
  setExit();
  return index;
}

// TW2 (2026-11-14): the system's Chinese fonts as the fallbacks of the text functions (prepass_screen.h): the
// standard font every screen of the port loads is the Japanese / Western one and has no Simplified Chinese. pl has to
// be initialised by the caller and stay so while the text is drawn (the fonts are its shared memory).
inline void UseSystemFallbackFonts() {
  PlFontData zh{}, ext{};
  const bool a = R_SUCCEEDED(plGetSharedFontByType(&zh, PlSharedFontType_ChineseSimplified));
  const bool b = R_SUCCEEDED(plGetSharedFontByType(&ext, PlSharedFontType_ExtChineseSimplified));
  SetFallbackFonts(a ? static_cast<const uint8_t *>(zh.address) : nullptr, a ? zh.size : 0,
                   b ? static_cast<const uint8_t *>(ext.address) : nullptr, b ? ext.size : 0);
}

}  // namespace renut::nxs

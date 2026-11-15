// tw2de (2026-10-23): the achievement popup's image, drawn by the CPU (as xc2fsr's menu draws its panel: a small RGBA
// image of a box and text from a bitmap font, put over the top left corner of the picture). No GPU or platform code
// here, so it can be tested on any host (tools: none needed beyond a C++ compiler).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace renut::gpu::popup {

// The popup's header in the game's language (XLanguage: 1 English, 3 German, 4 French, 5 Spanish, 6 Italian; any other
// gets English).
std::string_view HeaderText(uint32_t language);

struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // straight (not premultiplied) alpha, rows of width * 4 bytes
};

// tw2de (2026-10-28): a font made at run time (the system's font, rasterised for the code points a popup needs) in
// the baked fonts' form (popup_font.h). Must hold '?' and U+2026 (the stand-in and the ellipsis): View() checks.
struct OwnedFont {
  int ascent = 0, descent = 0;
  std::vector<uint8_t> coverage;
  struct Entry {
    uint32_t codepoint;
    int advance, x, y, w, h;
    size_t offset;
  };
  std::vector<Entry> entries;
  // Adds a glyph (in any order; w x h coverage bytes).
  void Add(uint32_t codepoint, int advance, int x, int y, int w, int h, const uint8_t *bytes);
};

// The code points of UTF-8 text, each once, ascending (with '?' and U+2026, which the layout may need).
std::vector<uint32_t> CodePointsOf(std::string_view a, std::string_view b);

// The toast: the header, then "<gamerscore>G - <name>" (the name only when gamerscore is 0). name is UTF-8 (the game's
// XDBF strings); a character the font does not have is drawn as '?'. large: the 1080-line size, else the 720-line one.
Image Compose(std::string_view header, std::string_view name, uint32_t gamerscore, bool large);
// tw2de (2026-10-28): the same in 'font' (the second line's text: SecondLine). An empty image when the font lacks
// '?' or U+2026 or a glyph does not fit the baked form's fields: the caller then uses the baked font.
Image Compose(std::string_view header, std::string_view name, uint32_t gamerscore, bool large, const OwnedFont &font);
// "<gamerscore>G - <name>", or the name alone when gamerscore is 0.
std::string SecondLine(std::string_view name, uint32_t gamerscore);

}  // namespace renut::gpu::popup

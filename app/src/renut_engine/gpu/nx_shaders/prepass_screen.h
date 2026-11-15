// tw2de (2026-10-21): the shader pre-pass's screen: the game's own dashboard art (the nxeart package in the game's
// assets: an Xbox 360 STFS package with the background and the banner), its title, the step, and a progress
// bar. Drawn into a 1280x720 RGBA canvas; the platform shows it (shader_prepass_switch.cpp: libnx's framebuffer).
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace renut::nxs {

struct Image {
  int width = 0, height = 0;
  std::vector<uint32_t> rgba;  // R in the low byte
  bool Empty() const { return rgba.empty(); }
};

// The files of an STFS package (LIVE / PIRS / CON) by name, and its header's thumbnail. Read-only, contiguous files.
class StfsPackage {
 public:
  bool Open(const std::string &path);
  bool Read(const std::string &name, std::vector<uint8_t> &out) const;
  bool Thumbnail(std::vector<uint8_t> &png) const;

 private:
  uint64_t BlockOffset(uint32_t block) const;
  std::vector<uint8_t> data_;
  uint32_t tables_per_block_ = 1;
  struct Entry {
    std::string name;
    uint32_t start, blocks, size;
    bool contiguous;
  };
  std::vector<Entry> entries_;
};

Image DecodeImage(const std::vector<uint8_t> &bytes);  // JPEG or PNG
Image Resize(const Image &src, int width, int height);

struct ScreenText {
  std::string title;      // "Preparing shaders"
  std::string step;       // "Step 1 of 2 - Scanning the game files"
  std::string detail;     // counts
  std::string eta;        // "About 12 minutes left"
  std::string footer;     // the button
  float progress = 0.0f;  // 0..1, < 0: none
};

// tw2de (2026-10-22): one line of text as coverage (0..255) for the GPU's "Compiling shaders..." notice
// (backend/compile_notice.cpp): w x h bytes, the text at (pad, pad). Done once, at start-up.
bool RasterLine(const uint8_t *font, size_t font_size, const std::string &s, float px, int pad,
                std::vector<uint8_t> &coverage, int &w, int &h);

// tw2de (2026-10-28): single glyphs of a TrueType font at a pixel height (ascent + descent), as coverage, for the
// achievement popup (backend/achievement_popup.cpp) in the system's font. A code point the font does not have is left
// out. Advances rounded to whole pixels; ascent and descent (positive, below) in pixels.
struct GlyphBitmap {
  uint32_t codepoint = 0;
  int advance = 0, x = 0, y = 0, w = 0, h = 0;  // x, y: the bitmap's top left from the pen on the baseline
  std::vector<uint8_t> coverage;                // w x h
};
bool RasterGlyphs(const uint8_t *font, size_t font_size, float px, const std::vector<uint32_t> &codepoints,
                  int &ascent, int &descent, std::vector<GlyphBitmap> &out);
// TW2 (2026-11-14): up to two fonts every text function here (PrepassScreen's, RasterGlyphs, RasterLine) takes the
// code points from that its own font lacks: the system's Chinese fonts (ui_language.h UseSystemFallbackFonts). The
// bytes are the caller's and have to stay while text is drawn; null: none.
void SetFallbackFonts(const uint8_t *font0, size_t size0, const uint8_t *font1, size_t size1);

// tw2de (2026-10-25): the boot menu (boot_menu_switch.cpp) on the same background: rows of a label and a value,
// one selected; a disabled row is drawn dimmed (its value fixed). A row with no value is an action ("Start the game").
struct MenuRow {
  std::string label;
  std::string value;
  bool enabled = true;
};
struct MenuText {
  std::string title;
  std::vector<MenuRow> rows;
  int selected = 0;
  std::string note;    // under the rows: what the selected row does
  std::string footer;  // the buttons ('\n' between lines)
  // tw2de (2026-10-28): the achievement lists: the background without the logo (the rows from near the top, up to
  // 15 of them), and a selected row's value without the < > of a setting.
  bool plain = false;
  bool arrows = true;
};

class PrepassScreen {
 public:
  // art: the nxeart package (missing: a plain background). font: a TrueType font's bytes, kept by the caller.
  // tw2de (2026-11-07): scale 1 draws 1280x720, 1.5 1920x1080 (docked on a 1080p TV: ScreenWants1080p), the layout
  // and the text scaled with it. Init again to change it.
  bool Init(const std::string &art, const uint8_t *font, size_t font_size, float scale = 1.0f);
  int Width() const { return width_; }
  int Height() const { return height_; }
  // Draws into dst (Width() x Height() RGBA, stride in pixels).
  void Draw(uint32_t *dst, uint32_t stride, const ScreenText &text);
  // tw2de (2026-10-25): the boot menu, into dst as Draw.
  void DrawMenu(uint32_t *dst, uint32_t stride, const MenuText &menu);

 private:
  void Text(uint32_t *dst, uint32_t stride, const std::string &s, float px, int x, int y, uint32_t rgb, bool centre);
  int TextWidth(const std::string &s, float px);
  // tw2de (2026-10-28): from a Text call's y, the top of its tallest letters and the bottom of its deepest.
  void TextInk(float px, int &top, int &bottom);
  static constexpr int kMenuBannerY = 16;  // the boot menu's logo, near the top (the pre-pass screen's: 58), at scale 1
  // tw2de (2026-11-07): a length of the 1280x720 layout at this scale.
  int S(float v) const { return int(std::lround(v * scale_)); }
  float scale_ = 1.0f;
  int width_ = 1280, height_ = 720;
  std::vector<uint32_t> base_;  // the background with the banner, drawn once
  std::vector<uint32_t> plain_;  // tw2de (2026-10-28): the background alone (MenuText::plain)
  std::vector<uint32_t> menu_;   // tw2de (2026-10-28): the boot menu's, the logo at kMenuBannerY
  int menu_banner_bottom_ = 0;   // below the boot menu's logo (0: none)
  std::vector<uint8_t> font_info_;  // stbtt_fontinfo
  bool font_ok_ = false;
};

}  // namespace renut::nxs

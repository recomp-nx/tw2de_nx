// tw2de (2026-10-25): a language pack's own characters (Polish ą, ł, ...) drawn by the game's fonts, each in place
// of a character of the game's Western page (Windows-1252) it does not need.
//
// In the pack (lang/<name>.zip, tw2de_lang_pack.cpp), glyphs/glyphs.ini, one section per new character:
//   [ą]
//   replaces = ¤          the game's character it takes the place of (or its code: 0xA4)
//   16 = a_ogonek_16.png  its picture for each font size (16, 18, 28; a size without one keeps the old character)
//   18 = a_ogonek_18.png
//   28 = a_ogonek_28.png
//   advance = 0           extra space after it (pixels, may be negative)
//   offset = 0, 0         moves it right / down in its box (pixels)
// A picture is white on black (or white on transparent), as tools/font_export.py writes the game's own: its width is
// the character's width, its height at most the font's line height (16: 23, 18: 24, 28: 37 pixels). The bearings
// (space before and after) are the replaced character's, moved by offset and advance.
// The pack's texts are written with the new characters; they become the replaced characters' codes (GlyphCode).
//
// The fonts (gui/resident/system.x360.xgr, a WPD list, and its .imgb): wfnt16 / 18 / 28, DXT1 atlases (1024x1024,
// 2048x1024, 2048x2048, tiled the Xbox 360 way, 16-bit big-endian words), and their tables wfnt16t / 18t / 28t: a
// 32-byte header (u32 little-endian; its fifth word the number of kerning pairs), then per character of the Western
// page (code 0x20 first) 4 bytes of metrics (s8 left bearing, u8 width, s8 right bearing, 0), the kerning pairs (4
// bytes: two codes, s8 adjustment, 0), the characters' places in the atlas (u16 x, y, little-endian, code 0x20
// first), ..., and at the table's end the places of a second set (Japanese style: ¥ for \). tw2de_049.log: the game
// draws the first set (the second's places changed, the old characters were still drawn); both are changed. The new pictures go to a free part of the atlas (the end of a row of the Western page, blank in the
// game's data: wfnt16 y 831 from x 292, wfnt18 y 566 from x 1324, wfnt28 y 1151 from x 500), and the replaced
// characters' metrics and places are changed to them.
// Both are changed in guest memory: the tables when the file's data arrives (tw2de_wpd_patch.cpp), the atlas at Renut's first upload of it. Each only when it is
// the game's own (its XXH3 hash, of the release's files): anything else is left alone, and logged.
#include "cpu/tw2de_glyphs.h"

#include "cpu/tw2de_lang_pack.h"
#include "cpu/tw2de_wpd_patch.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <xxhash.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#include "stb_image.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tw2de {
namespace {

struct Font {
  int size;
  const char *table;  // its name in the WPD list
  uint32_t line;      // line height (the table header's third word)
  uint32_t width, height;
  uint64_t table_hash, atlas_hash;  // XXH3 of the release's table and atlas
  uint32_t free_x, free_y;          // where the new pictures go
};
constexpr Font kFonts[] = {
    {16, "wfnt16t", 23, 1024, 1024, 0x2fb84fbf1a18ffbeull, 0x0fc4ec0ba8c3e772ull, 292, 831},
    {18, "wfnt18t", 24, 2048, 1024, 0x3e160525cb792540ull, 0x86cd4fdf4856b431ull, 1324, 566},
    {28, "wfnt28t", 37, 2048, 2048, 0x2d75ae7f59470163ull, 0xfb2e11e4dc98a986ull, 500, 1151},
};
constexpr int kFontCount = 3;

struct Glyph {
  int code = 0;                 // the replaced character's
  int advance = 0, dx = 0, dy = 0;
  uint32_t w = 0, h = 0;        // picture (h at most the line)
  std::vector<uint8_t> pixels;  // w x h, 0 black .. 255 white
  uint32_t x = 0, y = 0;        // its place in the atlas
};
struct FontPatch {
  std::vector<Glyph> glyphs;
  bool table_done = false, atlas_done = false;
};

FontPatch g_fonts[kFontCount];
std::unordered_map<char32_t, int> g_codes;  // new character -> replaced code
std::atomic<bool> g_active{false};

std::mutex g_mutex;

// ---- text ----

std::string Trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
    ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
    --b;
  return s.substr(a, b - a);
}

// The UTF-8 string's characters.
std::u32string Utf8(const std::string &s) {
  std::u32string out;
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = uint8_t(s[i]);
    const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    char32_t v = n == 1 ? c : n == 2 ? (c & 31) : n == 3 ? (c & 15) : (c & 7);
    for (int k = 1; k < n && i + k < s.size(); ++k)
      v = (v << 6) | (uint8_t(s[i + k]) & 63);
    out.push_back(v);
    i += size_t(n);
  }
  return out;
}

// Windows-1252's code of a character (0x20-0xFF), or -1.
int Cp1252(char32_t c) {
  static const char16_t k80[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                   0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                   0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
  if ((c >= 0x20 && c < 0x80) || (c >= 0xA0 && c <= 0xFF))
    return int(c);
  for (int i = 0; i < 32; ++i)
    if (k80[i] && k80[i] == c)
      return 0x80 + i;
  return -1;
}

// ---- the atlas: DXT1 blocks, tiled ----

uint32_t TiledBlock(uint32_t x, uint32_t y, uint32_t width_blocks) {  // the block's index (8-byte blocks)
  const uint32_t aw = (width_blocks + 31) & ~31u, lb = 3;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb;
}

int Lum(uint32_t c) {
  const int r = int((c >> 11) & 31) * 255 / 31, g = int((c >> 5) & 63) * 255 / 63, b = int(c & 31) * 255 / 31;
  return (r * 299 + g * 587 + b * 114) / 1000;
}
uint32_t Gray(int l) { return uint32_t(l >> 3) << 11 | uint32_t(l >> 2) << 5 | uint32_t(l >> 3); }

void DecodeBlock(const uint8_t *mem, int out[16]) {
  uint8_t b[8];
  for (int i = 0; i < 8; ++i)
    b[i] = mem[i ^ 1];
  const uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
  const uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
  const int l0 = Lum(c0), l1 = Lum(c1);
  const int pal[4] = {l0, l1, c0 > c1 ? (2 * l0 + l1) / 3 : (l0 + l1) / 2, c0 > c1 ? (l0 + 2 * l1) / 3 : 0};
  for (int i = 0; i < 16; ++i)
    out[i] = pal[(bits >> (2 * i)) & 3];
}

void EncodeBlock(const int v[16], uint8_t *mem) {
  int mx = 0, mn = 255;
  for (int i = 0; i < 16; ++i) {
    mx = std::max(mx, v[i]);
    mn = std::min(mn, v[i]);
  }
  const uint32_t c0 = Gray(mx), c1 = Gray(mn);
  uint32_t bits = 0;
  if (c0 != c1) {  // c0 > c1: the 4-colour mode
    const int l0 = Lum(c0), l1 = Lum(c1);
    const int pal[4] = {l0, l1, (2 * l0 + l1) / 3, (l0 + 2 * l1) / 3};
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      for (int k = 1; k < 4; ++k)
        if (std::abs(pal[k] - v[i]) < std::abs(pal[best] - v[i]))
          best = k;
      bits |= uint32_t(best) << (2 * i);
    }
  }
  const uint8_t b[8] = {uint8_t(c0), uint8_t(c0 >> 8), uint8_t(c1), uint8_t(c1 >> 8),
                        uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16), uint8_t(bits >> 24)};
  for (int i = 0; i < 8; ++i)
    mem[i ^ 1] = b[i];
}

// The glyph's box (its width x the line) written into the atlas: its picture, moved down by dy, black around it.
void DrawGlyph(uint8_t *data, const Font &font, const Glyph &g) {
  const uint32_t wb = font.width / 4;
  for (uint32_t by = g.y / 4; by <= (g.y + font.line - 1) / 4; ++by)
    for (uint32_t bx = g.x / 4; bx <= (g.x + g.w - 1) / 4; ++bx) {
      uint8_t *mem = data + size_t(TiledBlock(bx, by, wb)) * 8;
      int v[16];
      DecodeBlock(mem, v);
      for (int i = 0; i < 16; ++i) {
        const int px = int(bx * 4 + (i & 3)) - int(g.x), py = int(by * 4 + (i >> 2)) - int(g.y);
        if (px < 0 || py < 0 || px >= int(g.w) || py >= int(font.line))
          continue;
        const int sy = py - g.dy;
        v[i] = sy >= 0 && sy < int(g.h) ? g.pixels[size_t(sy) * g.w + size_t(px)] : 0;
      }
      EncodeBlock(v, mem);
    }
}

// ---- the tables ----

uint32_t Be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void PatchTable(uint8_t *t, uint32_t size, int f) {
  const Font &font = kFonts[f];
  FontPatch &patch = g_fonts[f];
  if (XXH3_64bits(t, size) != font.table_hash) {
    REXLOG_WARN("[glyphs] {}: not the release's table (hash {:016X}): left alone", font.table, XXH3_64bits(t, size));
    return;
  }
  const uint32_t n = (size - 32) / 4;
  const uint32_t kerning = uint32_t(t[16]) | uint32_t(t[17]) << 8 | uint32_t(t[18]) << 16 | uint32_t(t[19]) << 24;
  for (const Glyph &g : patch.glyphs) {
    const uint32_t k = uint32_t(g.code - 0x20);
    uint8_t *m = t + 32 + 4 * k;
    m[0] = uint8_t(int8_t(std::clamp(int(int8_t(m[0])) + g.dx, -128, 127)));
    m[1] = uint8_t(g.w);
    m[2] = uint8_t(int8_t(std::clamp(int(int8_t(m[2])) + g.advance, -128, 127)));
    for (const uint32_t place : {224 + kerning + k, n - 224 + k}) {  // the Western set's, the second set's
      uint8_t *p = t + 32 + 4 * place;
      p[0] = uint8_t(g.x);
      p[1] = uint8_t(g.x >> 8);
      p[2] = uint8_t(g.y);
      p[3] = uint8_t(g.y >> 8);
    }
  }
  patch.table_done = true;
  REXLOG_INFO("[glyphs] {}: {} characters replaced", font.table, patch.glyphs.size());
}

// ---- glyphs.ini ----

bool LoadGlyph(const LangPack &pack, const std::string &file, Glyph &g, const Font &font, const std::string &section) {
  const std::vector<uint8_t> *png = pack.Find("glyphs/" + file);
  if (!png) {
    REXLOG_ERROR("[glyphs] [{}]: glyphs/{} is not in the pack", section, file);
    return false;
  }
  int w = 0, h = 0, n = 0;
  stbi_uc *p = stbi_load_from_memory(png->data(), int(png->size()), &w, &h, &n, 4);
  if (!p) {
    REXLOG_ERROR("[glyphs] [{}]: glyphs/{} is not a PNG", section, file);
    return false;
  }
  if (w < 1 || w > 64) {
    REXLOG_ERROR("[glyphs] [{}]: glyphs/{} is {} pixels wide (1-64)", section, file, w);
    stbi_image_free(p);
    return false;
  }
  if (uint32_t(h) > font.line)
    REXLOG_WARN("[glyphs] [{}]: glyphs/{} is {} pixels high, the {} font's line {}: cut", section, file, h, font.size,
                font.line);
  g.w = uint32_t(w);
  g.h = std::min(uint32_t(h), font.line);
  g.pixels.resize(size_t(g.w) * g.h);
  for (size_t i = 0; i < g.pixels.size(); ++i) {
    const stbi_uc *q = p + i * 4;
    g.pixels[i] = uint8_t(int(std::max({q[0], q[1], q[2]})) * q[3] / 255);
  }
  stbi_image_free(p);
  return true;
}

void ParseIni(const LangPack &pack, const std::string &text) {
  struct Section {
    std::string name;
    std::unordered_map<std::string, std::string> keys;
  };
  std::vector<Section> sections;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos)
      end = text.size();
    std::string line = Trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line[0] == ';' || line[0] == '#')
      continue;
    if (line.front() == '[' && line.back() == ']') {
      sections.push_back({Trim(line.substr(1, line.size() - 2)), {}});
    } else if (const size_t eq = line.find('='); eq != std::string::npos && !sections.empty()) {
      sections.back().keys[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
  }
  uint32_t cursor[kFontCount];
  for (int f = 0; f < kFontCount; ++f)
    cursor[f] = kFonts[f].free_x;
  for (const Section &s : sections) {
    const std::u32string ch = Utf8(s.name);
    if (ch.size() != 1) {
      REXLOG_ERROR("[glyphs] [{}]: a section is one character", s.name);
      continue;
    }
    auto get = [&](const char *key) {
      auto it = s.keys.find(key);
      return it == s.keys.end() ? std::string() : it->second;
    };
    const std::string rep = get("replaces");
    int code = -1;
    if (rep.size() > 2 && rep[0] == '0' && (rep[1] == 'x' || rep[1] == 'X'))
      code = int(std::strtol(rep.c_str() + 2, nullptr, 16));
    else if (const std::u32string r = Utf8(rep); r.size() == 1)
      code = Cp1252(r[0]);
    if (code < 0x21 || code > 0xFF) {
      REXLOG_ERROR("[glyphs] [{}]: replaces = \"{}\": not a character of the game's fonts (0x21-0xFF)", s.name, rep);
      continue;
    }
    Glyph base;
    base.code = code;
    base.advance = std::atoi(get("advance").c_str());
    if (const std::string o = get("offset"); !o.empty()) {
      base.dx = std::atoi(o.c_str());
      if (const size_t comma = o.find(','); comma != std::string::npos)
        base.dy = std::atoi(o.c_str() + comma + 1);
    }
    int sizes = 0;
    for (int f = 0; f < kFontCount; ++f) {
      const std::string file = get(std::to_string(kFonts[f].size).c_str());
      if (file.empty())
        continue;
      Glyph g = base;
      if (!LoadGlyph(pack, file, g, kFonts[f], s.name))
        continue;
      if (cursor[f] + g.w > kFonts[f].width) {
        REXLOG_ERROR("[glyphs] [{}]: no room left in the {} font", s.name, kFonts[f].size);
        continue;
      }
      g.x = cursor[f];
      g.y = kFonts[f].free_y;
      cursor[f] = (cursor[f] + g.w + 4 + 3) & ~3u;
      g_fonts[f].glyphs.push_back(std::move(g));
      ++sizes;
    }
    if (sizes)
      g_codes[ch[0]] = code;
  }
}

}  // namespace

bool DecodePng(const std::vector<uint8_t> &png, int &width, int &height, std::vector<uint8_t> &rgba) {
  int n = 0;
  stbi_uc *p = stbi_load_from_memory(png.data(), int(png.size()), &width, &height, &n, 4);
  if (!p)
    return false;
  rgba.assign(p, p + size_t(width) * size_t(height) * 4);
  stbi_image_free(p);
  return true;
}

void GlyphsInit() {
  const LangPack *pack = GetLangPack();
  if (!pack)
    return;
  const std::vector<uint8_t> *ini = pack->Find("glyphs/glyphs.ini");
  if (!ini)
    return;
  std::string text(ini->begin(), ini->end());
  if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    text.erase(0, 3);  // a UTF-8 BOM
  ParseIni(*pack, text);
  size_t total = 0;
  for (const FontPatch &p : g_fonts)
    total += p.glyphs.size();
  REXLOG_INFO("[glyphs] {}: {} new characters ({} pictures)", pack->name, g_codes.size(), total);
  // The tables, when their file is loaded (tw2de_wpd_patch.cpp).
  for (int f = 0; f < kFontCount; ++f)
    if (!g_fonts[f].glyphs.empty())
      WpdPatchRegister(kFonts[f].table, "wfl", [f](uint8_t *data, uint32_t size) {
        std::lock_guard<std::mutex> lock(g_mutex);
        PatchTable(data, size, f);
      });
  g_active.store(total != 0, std::memory_order_release);
}

bool GlyphsActive() { return g_active.load(std::memory_order_acquire); }

int GlyphCode(char32_t c) {
  auto it = g_codes.find(c);
  return it == g_codes.end() ? -1 : it->second;
}

void GlyphsPatchAtlas(uint8_t *data, uint32_t width, uint32_t height) {
  if (!GlyphsActive())
    return;
  for (int f = 0; f < kFontCount; ++f) {
    const Font &font = kFonts[f];
    FontPatch &patch = g_fonts[f];
    if (font.width != width || font.height != height || patch.glyphs.empty())
      continue;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (patch.atlas_done)
      return;
    const uint64_t hash = XXH3_64bits(data, size_t(width) * height / 2);
    if (hash != font.atlas_hash) {
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 8)
        REXLOG_INFO("[glyphs] a {}x{} DXT1 texture, not the {} font (hash {:016X})", width, height, font.size, hash);
      return;
    }
    for (const Glyph &g : patch.glyphs)
      DrawGlyph(data, font, g);
    patch.atlas_done = true;
    REXLOG_INFO("[glyphs] wfnt{} atlas: {} characters drawn{}", font.size, patch.glyphs.size(),
                patch.table_done ? "" : " (its table not changed yet)");
    return;
  }
}

}  // namespace tw2de

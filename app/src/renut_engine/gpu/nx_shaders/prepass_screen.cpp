// tw2de (2026-10-21): prepass_screen.h.
#include "renut_engine/gpu/nx_shaders/prepass_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
// Third-party: GCC's -Wstringop-overflow misreads stbi__parse_png_file's palette loop.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#include "stb_image.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"  // tw2de (2026-10-22): Dear ImGui's copy (imstb_truetype.h), kept here alone

namespace renut::nxs {

namespace {

inline uint32_t Be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint32_t Le24(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }

inline uint32_t Rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xFF000000u | (b << 16) | (g << 8) | r; }

inline uint32_t Blend(uint32_t dst, uint32_t rgb, uint32_t a) {
  const uint32_t ia = 255 - a;
  const uint32_t r = ((dst & 0xFF) * ia + (rgb & 0xFF) * a) / 255;
  const uint32_t g = (((dst >> 8) & 0xFF) * ia + ((rgb >> 8) & 0xFF) * a) / 255;
  const uint32_t b = (((dst >> 16) & 0xFF) * ia + ((rgb >> 16) & 0xFF) * a) / 255;
  return 0xFF000000u | (b << 16) | (g << 8) | r;
}

constexpr uint32_t kInk = 0x3A2F2Bu;      // the text (0xBBGGRR)
constexpr uint32_t kInkSoft = 0x70605Au;
constexpr uint32_t kBarBack = 0xD8D2D0u;
constexpr uint32_t kBarFill = 0xA0A73Fu;  // The game's crystal teal
constexpr uint32_t kBarEdge = 0x8A7F7Au;

}  // namespace

// ---- STFS

bool StfsPackage::Open(const std::string &path) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0xC000 || size > (64 << 20)) {
    std::fclose(f);
    return false;
  }
  data_.resize(size_t(size));
  const bool read = std::fread(data_.data(), 1, data_.size(), f) == data_.size();
  std::fclose(f);
  if (!read || !(std::memcmp(data_.data(), "LIVE", 4) == 0 || std::memcmp(data_.data(), "PIRS", 4) == 0 ||
                 std::memcmp(data_.data(), "CON ", 4) == 0))
    return false;
  const uint8_t *vd = data_.data() + 0x379;
  tables_per_block_ = (vd[2] & 1) ? 1 : 2;
  const uint32_t table_blocks = uint32_t(vd[3]) | (uint32_t(vd[4]) << 8);
  const uint32_t table_start = Le24(vd + 5);
  entries_.clear();
  for (uint32_t b = 0; b < table_blocks; b++) {
    const uint64_t at = BlockOffset(table_start + b);
    if (at + 0x1000 > data_.size())
      break;
    for (uint32_t i = 0; i < 0x1000 / 0x40; i++) {
      const uint8_t *e = data_.data() + at + i * 0x40;
      const uint32_t len = e[0x28] & 0x3F;
      if (len == 0 || len > 0x28)
        return !entries_.empty();
      Entry entry;
      entry.name.assign(reinterpret_cast<const char *>(e), len);
      entry.contiguous = (e[0x28] & 0x40) != 0;
      entry.blocks = Le24(e + 0x29);
      entry.start = Le24(e + 0x2F);
      entry.size = Be32(e + 0x34);
      if (!(e[0x28] & 0x80))
        entries_.push_back(entry);
    }
  }
  return !entries_.empty();
}

uint64_t StfsPackage::BlockOffset(uint32_t block) const {
  // The data blocks with the hash tables between them: one (or two) before every 170 blocks, and the higher levels'.
  const uint64_t header = Be32(data_.data() + 0x340);
  uint64_t b = block;
  b += (uint64_t(block) / 170 + 1) * tables_per_block_;
  if (block >= 170)
    b += (uint64_t(block) / 28900 + 1) * tables_per_block_;
  if (block >= 28900)
    b += tables_per_block_;
  return ((header + 0xFFF) & ~uint64_t(0xFFF)) + (b << 12);
}

bool StfsPackage::Read(const std::string &name, std::vector<uint8_t> &out) const {
  for (const Entry &e : entries_) {
    if (e.name != name)
      continue;
    if (!e.contiguous)
      return false;
    out.clear();
    out.reserve(e.size);
    for (uint32_t k = 0; out.size() < e.size; k++) {
      const uint64_t at = BlockOffset(e.start + k);
      const size_t n = std::min<size_t>(0x1000, e.size - out.size());
      if (at + n > data_.size())
        return false;
      out.insert(out.end(), data_.begin() + long(at), data_.begin() + long(at + n));
    }
    return true;
  }
  return false;
}

bool StfsPackage::Thumbnail(std::vector<uint8_t> &png) const {
  if (data_.size() < 0x571A)
    return false;
  const uint32_t size = Be32(data_.data() + 0x1712);
  if (size == 0 || size > 0x3C00)
    return false;
  png.assign(data_.begin() + 0x171A, data_.begin() + 0x171A + size);
  return true;
}

// ---- images

Image DecodeImage(const std::vector<uint8_t> &bytes) {
  Image img;
  int w = 0, h = 0, n = 0;
  stbi_uc *p = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
  if (!p)
    return img;
  img.width = w;
  img.height = h;
  img.rgba.resize(size_t(w) * h);
  std::memcpy(img.rgba.data(), p, img.rgba.size() * 4);
  stbi_image_free(p);
  return img;
}

// tw2de (2026-10-22): Lanczos-3, separable, in linear light (the box filter before averaged gamma-encoded values
// over uneven, overlapping spans at 1.5:1: soft and blotchy). Each output pixel weighs the source pixels within 3 of
// its own widths (Lanczos stretched by the ratio when shrinking, so nothing aliases), the weights normalised; the
// colour is decoded from sRGB first and encoded again after.
namespace {

struct Taps {
  std::vector<int> first;     // per output pixel: its first source pixel
  std::vector<int> count;     // and how many
  std::vector<float> weight;  // count weights per output pixel, at offset[i]
  std::vector<int> offset;
};

float Lanczos3(float x) {
  x = std::fabs(x);
  if (x < 1e-6f)
    return 1.0f;
  if (x >= 3.0f)
    return 0.0f;
  const float pi_x = 3.14159265358979f * x;
  return 3.0f * std::sin(pi_x) * std::sin(pi_x / 3.0f) / (pi_x * pi_x);
}

Taps MakeTaps(int src, int dst) {
  Taps t;
  const float ratio = float(src) / float(dst);
  const float stretch = std::max(1.0f, ratio);  // shrinking: the kernel as wide as an output pixel
  const float support = 3.0f * stretch;
  t.first.resize(size_t(dst));
  t.count.resize(size_t(dst));
  t.offset.resize(size_t(dst));
  for (int i = 0; i < dst; i++) {
    const float center = (float(i) + 0.5f) * ratio;  // in source pixels, edges at integers
    const int lo = int(std::floor(center - support)), hi = int(std::ceil(center + support));
    t.first[size_t(i)] = lo;
    t.count[size_t(i)] = hi - lo;
    t.offset[size_t(i)] = int(t.weight.size());
    float sum = 0.0f;
    for (int j = lo; j < hi; j++) {
      const float w = Lanczos3((float(j) + 0.5f - center) / stretch);
      t.weight.push_back(w);
      sum += w;
    }
    for (int k = 0; k < hi - lo; k++)
      t.weight[size_t(t.offset[size_t(i)] + k)] /= sum;
  }
  return t;
}

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float LinearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

}  // namespace

Image Resize(const Image &src, int width, int height) {
  Image out;
  if (src.Empty() || width <= 0 || height <= 0)
    return out;
  out.width = width;
  out.height = height;
  out.rgba.resize(size_t(width) * height);
  float to_linear[256];
  for (int i = 0; i < 256; i++)
    to_linear[i] = SrgbToLinear(float(i) / 255.0f);
  constexpr int kLut = 4096;  // linear -> sRGB byte
  std::vector<uint8_t> to_srgb(kLut + 1);
  for (int i = 0; i <= kLut; i++)
    to_srgb[size_t(i)] = uint8_t(std::lround(LinearToSrgb(float(i) / kLut) * 255.0f));
  auto encode = [&](float v) { return uint32_t(to_srgb[size_t(std::lround(std::clamp(v, 0.0f, 1.0f) * kLut))]); };
  const Taps tx = MakeTaps(src.width, width), ty = MakeTaps(src.height, height);
  // Horizontal: each source row to width pixels, linear RGBA floats.
  std::vector<float> rows(size_t(width) * src.height * 4);
  for (int y = 0; y < src.height; y++) {
    const uint32_t *in = src.rgba.data() + size_t(y) * src.width;
    float *o = rows.data() + size_t(y) * width * 4;
    for (int x = 0; x < width; x++) {
      float acc[4] = {};
      const float *w = tx.weight.data() + tx.offset[size_t(x)];
      for (int k = 0; k < tx.count[size_t(x)]; k++) {
        const int sx = std::clamp(tx.first[size_t(x)] + k, 0, src.width - 1);
        const uint32_t p = in[sx];
        acc[0] += w[k] * to_linear[p & 0xFF];
        acc[1] += w[k] * to_linear[(p >> 8) & 0xFF];
        acc[2] += w[k] * to_linear[(p >> 16) & 0xFF];
        acc[3] += w[k] * float(p >> 24) / 255.0f;
      }
      std::memcpy(o + size_t(x) * 4, acc, sizeof(acc));
    }
  }
  // Vertical: to height rows, encoded again.
  for (int y = 0; y < height; y++) {
    const float *w = ty.weight.data() + ty.offset[size_t(y)];
    for (int x = 0; x < width; x++) {
      float acc[4] = {};
      for (int k = 0; k < ty.count[size_t(y)]; k++) {
        const int sy = std::clamp(ty.first[size_t(y)] + k, 0, src.height - 1);
        const float *p = rows.data() + (size_t(sy) * width + x) * 4;
        acc[0] += w[k] * p[0];
        acc[1] += w[k] * p[1];
        acc[2] += w[k] * p[2];
        acc[3] += w[k] * p[3];
      }
      const uint32_t a = uint32_t(std::lround(std::clamp(acc[3], 0.0f, 1.0f) * 255.0f));
      out.rgba[size_t(y) * width + x] = encode(acc[0]) | (encode(acc[1]) << 8) | (encode(acc[2]) << 16) | (a << 24);
    }
  }
  return out;
}

// ---- the screen

bool PrepassScreen::Init(const std::string &art, const uint8_t *font, size_t font_size, float scale) {
  scale_ = scale;
  width_ = S(1280.0f);
  height_ = S(720.0f);
  menu_banner_bottom_ = 0;
  plain_.clear();
  menu_.clear();
  font_info_.clear();
  font_ok_ = false;
  base_.assign(size_t(width_) * height_, 0);
  // A light gradient like the art's, for a missing package.
  for (int y = 0; y < height_; y++) {
    const uint32_t v = 0xF2 - uint32_t(y * 0x12 / height_);
    std::fill(base_.begin() + long(y) * width_, base_.begin() + long(y + 1) * width_, Rgb(v, v - 1, v + 1));
  }
  StfsPackage package;
  if (package.Open(art)) {
    std::vector<uint8_t> bytes;
    if (package.Read("nxebg.jpg", bytes)) {
      Image bg = Resize(DecodeImage(bytes), width_, height_);
      if (!bg.Empty())
        for (size_t i = 0; i < base_.size(); i++)
          base_[i] = bg.rgba[i] | 0xFF000000u;
    }
    plain_ = base_;  // tw2de (2026-10-28): before the banner
    if (package.Read("nxeslot.jpg", bytes)) {
      // The banner: the logo on a grey gradient of its own, which is keyed out (each row's background is its edge
      // pixels' colour; a pixel close to it is transparent) and its edges faded, so the logo sits on the picture.
      // tw2de (2026-10-28): on the pre-pass screen at y 58; on the boot menu's (menu_) near the top, y 16.
      Image banner = DecodeImage(bytes);
      if (!banner.Empty() && scale_ != 1.0f)
        banner = Resize(banner, S(float(banner.width)), S(float(banner.height)));  // tw2de (2026-11-07)
      if (!banner.Empty()) {
        menu_ = base_;
        auto put = [&](std::vector<uint32_t> &into, int y0) {
          const int x0 = (width_ - banner.width) / 2;
          const float feather = 24.0f * scale_;
          for (int y = 0; y < banner.height; y++) {
            const uint32_t *row = banner.rgba.data() + size_t(y) * banner.width;
            const uint32_t l = row[2], r = row[banner.width - 3];
            for (int x = 0; x < banner.width; x++) {
              const float t = float(x) / float(banner.width - 1);
              const uint32_t p = row[x];
              float diff = 0.0f;
              for (int c = 0; c < 3; c++) {
                const float bgc = float((l >> (8 * c)) & 0xFF) * (1.0f - t) + float((r >> (8 * c)) & 0xFF) * t;
                diff = std::max(diff, std::fabs(float((p >> (8 * c)) & 0xFF) - bgc));
              }
              float a = std::clamp((diff - 6.0f) / 34.0f, 0.0f, 1.0f);
              const float d = std::min(std::min(float(x), float(banner.width - 1 - x)),
                                       std::min(float(y), float(banner.height - 1 - y)));
              a *= std::clamp(d / feather, 0.0f, 1.0f);
              const int px = x0 + x, py = y0 + y;
              if (px < 0 || py < 0 || px >= width_ || py >= height_ || a <= 0.0f)
                continue;
              uint32_t &dst = into[size_t(py) * width_ + px];
              dst = Blend(dst, p & 0xFFFFFF, uint32_t(a * 255.0f));
            }
          }
        };
        put(base_, S(58.0f));
        put(menu_, S(kMenuBannerY));
        menu_banner_bottom_ = S(kMenuBannerY) + banner.height;
      }
    }
  }
  const bool menu_own = !menu_.empty();
  if (plain_.empty())
    plain_ = base_;
  if (menu_.empty())
    menu_ = base_;
  // tw2de (2026-11-10): FF XIII-2's art (nxebg.jpg) is a busy grey photograph (feathers, the heroine), where FF
  // XIII's was a pale wash: the dark text on it was hard to read, the notes and buttons hardly at all. A light veil
  // behind the text: from just under the logo down (the logo and the top of the picture as they were), faded in over
  // 48 pixels; the achievement lists' background (no logo, the rows from the top) all of it.
  constexpr uint32_t kVeil = 0xEEF1F4u;  // 0xBBGGRR: a warm white
  constexpr uint32_t kVeilAlpha = 170;
  const auto veil = [&](std::vector<uint32_t> &img, int y0) {
    const int feather = S(48.0f);
    for (int y = std::max(0, y0 - feather); y < height_; y++) {
      const uint32_t a = y >= y0 ? kVeilAlpha : kVeilAlpha * uint32_t(y - (y0 - feather)) / uint32_t(feather);
      uint32_t *row = img.data() + size_t(y) * width_;
      for (int x = 0; x < width_; x++)
        row[x] = Blend(row[x], kVeil, a);
    }
  };
  veil(plain_, 0);
  veil(base_, S(380.0f));  // the pre-pass screen's text from y 404
  if (menu_own)
    veil(menu_, menu_banner_bottom_ ? menu_banner_bottom_ - S(8.0f) : 0);
  if (font && font_size) {

    font_info_.resize(sizeof(stbtt_fontinfo));
    auto *info = reinterpret_cast<stbtt_fontinfo *>(font_info_.data());
    font_ok_ = stbtt_InitFont(info, font, stbtt_GetFontOffsetForIndex(font, 0)) != 0;
  }
  return true;
}

namespace {
// The next code point of UTF-8 text.
uint32_t NextCodepoint(const std::string &s, size_t &i) {
  const uint8_t c = uint8_t(s[i++]);
  if (c < 0x80)
    return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
  uint32_t cp = c & (0x3F >> extra);
  while (extra-- > 0 && i < s.size())
    cp = (cp << 6) | (uint8_t(s[i++]) & 0x3F);
  return cp;
}

// TW2 (2026-11-14): fallback fonts (SetFallbackFonts: the system's Chinese fonts). The font every text function was
// given stays the first for everything it has; a code point it lacks comes from a fallback that has it. A line with a
// CJK character only a fallback has (Chinese text) takes every CJK character from the fallbacks where they have
// it: the first font is the Japanese one and has many of the same characters in its own shapes, and one typeface a
// line reads better. A line of the first font's characters alone is drawn exactly as before.
struct Fallback {
  stbtt_fontinfo info;
  bool ok = false;
};
Fallback g_fallback[2];

bool InFallback(uint32_t cp, const stbtt_fontinfo **font) {
  for (const Fallback &f : g_fallback) {
    if (f.ok && stbtt_FindGlyphIndex(&f.info, int(cp))) {
      if (font)
        *font = &f.info;
      return true;
    }
  }
  return false;
}
const stbtt_fontinfo *FontFor(const stbtt_fontinfo *first, uint32_t cp, bool cjk_first) {
  const stbtt_fontinfo *font = first;
  if (cjk_first && cp >= 0x2E80 && InFallback(cp, &font))
    return font;
  if (stbtt_FindGlyphIndex(first, int(cp)))
    return first;
  InFallback(cp, &font);
  return font;
}
bool CjkFirst(const stbtt_fontinfo *first, uint32_t cp) {
  return cp >= 0x2E80 && !stbtt_FindGlyphIndex(first, int(cp)) && InFallback(cp, nullptr);
}
bool CjkFirst(const stbtt_fontinfo *first, const std::string &s) {
  if (!g_fallback[0].ok && !g_fallback[1].ok)
    return false;
  for (size_t i = 0; i < s.size();)
    if (CjkFirst(first, NextCodepoint(s, i)))
      return true;
  return false;
}
}  // namespace

void SetFallbackFonts(const uint8_t *font0, size_t size0, const uint8_t *font1, size_t size1) {
  const uint8_t *fonts[2] = {font0, font1};
  const size_t sizes[2] = {size0, size1};
  for (int i = 0; i < 2; i++)
    g_fallback[i].ok = fonts[i] && sizes[i] &&
                       stbtt_InitFont(&g_fallback[i].info, fonts[i], stbtt_GetFontOffsetForIndex(fonts[i], 0)) != 0;
}

void PrepassScreen::TextInk(float px, int &top, int &bottom) {
  top = 0;
  bottom = int(px);
  if (!font_ok_)
    return;
  auto *info = reinterpret_cast<stbtt_fontinfo *>(font_info_.data());
  const float scale = stbtt_ScaleForPixelHeight(info, px);
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
  const float baseline = float(ascent) * scale;
  // The tallest letters the menus show (accented capitals) and the deepest (descenders, ogoneks): the fonts' own
  // ascent and descent can be less (the system font's descenders go below its descent).
  int y_max = 0, y_min = 0;
  static constexpr uint32_t kLetters[] = {0xD3, 0x15A, 0x17B, 0xC4, 'H', 'g', 'j', 'p', 'y', 0x119, 0x105};
  for (const uint32_t cp : kLetters) {
    int x0, y0, x1, y1;
    if (stbtt_GetCodepointBox(info, int(cp), &x0, &y0, &x1, &y1)) {
      y_max = std::max(y_max, y1);
      y_min = std::min(y_min, y0);
    }
  }
  top = int(std::floor(baseline - float(y_max) * scale));
  bottom = int(std::ceil(baseline - float(y_min) * scale));
}

int PrepassScreen::TextWidth(const std::string &s, float px) {
  if (!font_ok_)
    return 0;
  auto *info = reinterpret_cast<stbtt_fontinfo *>(font_info_.data());
  const bool cjk_first = CjkFirst(info, s);  // TW2 (2026-11-14): fallback fonts
  float w = 0;
  uint32_t prev = 0;
  const stbtt_fontinfo *prev_font = nullptr;
  for (size_t i = 0; i < s.size();) {
    const uint32_t cp = NextCodepoint(s, i);
    const stbtt_fontinfo *font = FontFor(info, cp, cjk_first);
    const float scale = stbtt_ScaleForPixelHeight(font, px);
    int advance, lsb;
    stbtt_GetCodepointHMetrics(font, int(cp), &advance, &lsb);
    if (prev && prev_font == font)
      w += scale * float(stbtt_GetCodepointKernAdvance(font, int(prev), int(cp)));
    w += scale * float(advance);
    prev = cp;
    prev_font = font;
  }
  return int(w + 0.5f);
}

void PrepassScreen::Text(uint32_t *dst, uint32_t stride, const std::string &s, float px, int x, int y, uint32_t rgb,
                         bool centre) {
  if (!font_ok_ || s.empty())
    return;
  auto *info = reinterpret_cast<stbtt_fontinfo *>(font_info_.data());
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
  const int baseline = y + int(float(ascent) * stbtt_ScaleForPixelHeight(info, px));
  const bool cjk_first = CjkFirst(info, s);  // TW2 (2026-11-14): fallback fonts
  float pen = float(centre ? x - TextWidth(s, px) / 2 : x);
  uint32_t prev = 0;
  const stbtt_fontinfo *prev_font = nullptr;
  for (size_t i = 0; i < s.size();) {
    const uint32_t cp = NextCodepoint(s, i);
    const stbtt_fontinfo *font = FontFor(info, cp, cjk_first);
    const float scale = stbtt_ScaleForPixelHeight(font, px);
    if (prev && prev_font == font)
      pen += scale * float(stbtt_GetCodepointKernAdvance(font, int(prev), int(cp)));
    int advance, lsb;
    stbtt_GetCodepointHMetrics(font, int(cp), &advance, &lsb);
    int w, h, xo, yo;
    unsigned char *bitmap =
        stbtt_GetCodepointBitmapSubpixel(font, scale, scale, pen - std::floor(pen), 0.0f, int(cp), &w, &h, &xo, &yo);
    if (bitmap) {
      const int gx = int(std::floor(pen)) + xo, gy = baseline + yo;
      for (int by = 0; by < h; by++) {
        const int py = gy + by;
        if (py < 0 || py >= height_)
          continue;
        for (int bx = 0; bx < w; bx++) {
          const int qx = gx + bx;
          const uint8_t a = bitmap[by * w + bx];
          if (qx < 0 || qx >= width_ || a == 0)
            continue;
          uint32_t &d = dst[size_t(py) * stride + qx];
          d = Blend(d, rgb, a);
        }
      }
      stbtt_FreeBitmap(bitmap, nullptr);
    }
    pen += scale * float(advance);
    prev = cp;
    prev_font = font;
  }
}

void PrepassScreen::Draw(uint32_t *dst, uint32_t stride, const ScreenText &t) {
  for (int y = 0; y < height_; y++)
    std::memcpy(dst + size_t(y) * stride, base_.data() + size_t(y) * width_, width_ * 4);
  const int cx = width_ / 2;
  Text(dst, stride, t.title, 38.0f * scale_, cx, S(404), kInk, true);
  Text(dst, stride, t.step, 26.0f * scale_, cx, S(456), kInk, true);
  if (t.progress >= 0.0f) {
    const int bx = S(240), by = S(506), bw = S(800), bh = S(16);
    const int fill = int(std::clamp(t.progress, 0.0f, 1.0f) * float(bw - 4));
    for (int y = by; y < by + bh; y++)
      for (int x = bx; x < bx + bw; x++) {
        const bool edge = y == by || y == by + bh - 1 || x == bx || x == bx + bw - 1;
        uint32_t &d = dst[size_t(y) * stride + x];
        if (edge)
          d = Blend(d, kBarEdge, 200);
        else if (x - bx - 2 < fill && y > by + 1 && y < by + bh - 2)
          d = Blend(d, kBarFill, 255);
        else
          d = Blend(d, kBarBack, 160);
      }
  }
  Text(dst, stride, t.detail, 22.0f * scale_, cx, S(538), kInkSoft, true);
  Text(dst, stride, t.eta, 22.0f * scale_, cx, S(570), kInkSoft, true);
  // The footer's lines ('\n' between them), one under the other.
  int fy = S(616);
  for (size_t at = 0; at <= t.footer.size(); fy += S(28)) {
    size_t nl = t.footer.find('\n', at);
    if (nl == std::string::npos)
      nl = t.footer.size();
    Text(dst, stride, t.footer.substr(at, nl - at), 20.0f * scale_, cx, fy, kInkSoft, true);
    at = nl + 1;
  }
}

// tw2de (2026-10-25): the boot menu (prepass_screen.h MenuText). Below the banner, as the pre-pass's text: the
// title, the rows (label right-aligned to the centre, value left of it; the selected row on a bar, its value between
// arrows), the selected row's note and the buttons.
void PrepassScreen::DrawMenu(uint32_t *dst, uint32_t stride, const MenuText &m) {
  const std::vector<uint32_t> &back = m.plain ? plain_ : menu_;
  for (int y = 0; y < height_; y++)
    std::memcpy(dst + size_t(y) * stride, back.data() + size_t(y) * width_, width_ * 4);
  const int cx = width_ / 2;
  // tw2de (2026-10-28): laid out from the top: the title under the logo (near the top of the screen; without it,
  // at the top), the rows under it as tall as the room above the note and the footer allows (28 to 38 pixels).
  const int title_y = m.plain || !menu_banner_bottom_ ? S(40) : menu_banner_bottom_ + S(8);
  Text(dst, stride, m.title, 32.0f * scale_, cx, title_y, kInk, true);
  int footer_lines = 1;
  for (char ch : m.footer)
    footer_lines += ch == '\n';
  const int top = title_y + S(46);
  const int room = height_ - top - S(8 + 30 + footer_lines * 24 + 12);
  const int n = std::max(int(m.rows.size()), 1);
  const int row_h = std::clamp(room / n, S(28), S(38));
  // The selection bar: from the accents above the capitals to the descenders, three pixels more each side, as far as
  // the row height allows (it never reaches the next row's letters).
  const float px = 24.0f * scale_;
  int ink_top = 0, ink_bottom = S(24);
  TextInk(px, ink_top, ink_bottom);
  const int mid = (ink_top + ink_bottom) / 2, bar_h = std::min(ink_bottom - ink_top + S(6), row_h - 1);
  // tw2de (2026-10-28): the bar spans what the rows hold, 32 pixels past it on each side, the same for every
  // row: from the longest label's start to the longest value's end (a selected setting's between its arrows), or an
  // action row's text; not centred on the screen when the labels are longer than the values (the achievement lists).
  int left = cx - S(120), right = cx + S(120);
  for (const MenuRow &r : m.rows) {
    if (r.value.empty()) {
      const int w = TextWidth(r.label, px);
      left = std::min(left, cx - w / 2);
      right = std::max(right, cx + w - w / 2);
      continue;
    }
    left = std::min(left, cx - S(24) - TextWidth(r.label, px));
    right = std::max(right, cx + S(24) + TextWidth(m.arrows ? "<   " + r.value + "   >" : r.value, px));
  }
  left = std::max(left - S(32), S(40));
  right = std::min(right + S(32), width_ - S(40));
  for (size_t i = 0; i < m.rows.size(); i++) {
    const MenuRow &r = m.rows[i];
    const int y = top + int(i) * row_h;
    const bool selected = int(i) == m.selected;
    if (selected) {
      for (int py = y + mid - bar_h / 2; py < y + mid - bar_h / 2 + bar_h; py++)
        for (int px = left; px < right; px++) {
          uint32_t &d = dst[size_t(py) * stride + px];
          d = Blend(d, kBarFill, 70);
        }
    }
    const uint32_t ink = r.enabled ? kInk : kInkSoft;
    if (r.value.empty()) {
      Text(dst, stride, r.label, px, cx, y, ink, true);
      continue;
    }
    Text(dst, stride, r.label, px, cx - S(24) - TextWidth(r.label, px), y, ink, false);
    const std::string value = selected && r.enabled && m.arrows ? "<   " + r.value + "   >" : r.value;
    Text(dst, stride, value, px, cx + S(24), y, ink, false);
  }
  const int note_y = top + int(m.rows.size()) * row_h + S(8);
  // tw2de (2026-10-25): a line wider than the screen's margins (the translations' longer notes) drawn smaller.
  const auto fit = [&](const std::string &line, float px) {
    const int w = TextWidth(line, px);
    const int kMax = width_ - S(80);
    return w > kMax ? px * float(kMax) / float(w) : px;
  };
  Text(dst, stride, m.note, fit(m.note, 19.0f * scale_), cx, note_y, kInkSoft, true);
  int fy = note_y + S(30);
  for (size_t at = 0; at <= m.footer.size(); fy += S(24)) {
    size_t nl = m.footer.find('\n', at);
    if (nl == std::string::npos)
      nl = m.footer.size();
    const std::string line = m.footer.substr(at, nl - at);
    Text(dst, stride, line, fit(line, 18.0f * scale_), cx, fy, kInkSoft, true);
    at = nl + 1;
  }
}

bool RasterGlyphs(const uint8_t *font, size_t font_size, float px, const std::vector<uint32_t> &codepoints,
                  int &ascent, int &descent, std::vector<GlyphBitmap> &out) {
  out.clear();
  if (!font || !font_size)
    return false;
  stbtt_fontinfo info;
  if (!stbtt_InitFont(&info, font, stbtt_GetFontOffsetForIndex(font, 0)))
    return false;
  int a, d, gap;
  stbtt_GetFontVMetrics(&info, &a, &d, &gap);
  ascent = int(std::ceil(float(a) * stbtt_ScaleForPixelHeight(&info, px)));
  descent = int(std::ceil(float(-d) * stbtt_ScaleForPixelHeight(&info, px)));
  bool cjk_first = false;  // TW2 (2026-11-14): fallback fonts
  for (const uint32_t cp : codepoints)
    cjk_first = cjk_first || CjkFirst(&info, cp);
  for (const uint32_t cp : codepoints) {
    const stbtt_fontinfo *use = FontFor(&info, cp, cjk_first);
    if (cp != ' ' && !stbtt_FindGlyphIndex(use, int(cp)))
      continue;
    const float scale = stbtt_ScaleForPixelHeight(use, px);
    GlyphBitmap g;
    g.codepoint = cp;
    int advance, lsb;
    stbtt_GetCodepointHMetrics(use, int(cp), &advance, &lsb);
    g.advance = int(std::lround(float(advance) * scale));
    int w = 0, h = 0, xo = 0, yo = 0;
    unsigned char *bitmap = stbtt_GetCodepointBitmap(use, scale, scale, int(cp), &w, &h, &xo, &yo);
    if (bitmap && w > 0 && h > 0) {
      g.x = xo;
      g.y = yo;
      g.w = w;
      g.h = h;
      g.coverage.assign(bitmap, bitmap + size_t(w) * h);
    }
    if (bitmap)
      stbtt_FreeBitmap(bitmap, nullptr);
    out.push_back(std::move(g));
  }
  return !out.empty();
}

bool RasterLine(const uint8_t *font, size_t font_size, const std::string &s, float px, int pad,
                std::vector<uint8_t> &coverage, int &w, int &h) {
  if (!font || !font_size || s.empty())
    return false;
  stbtt_fontinfo info;
  if (!stbtt_InitFont(&info, font, stbtt_GetFontOffsetForIndex(font, 0)))
    return false;
  const float first_scale = stbtt_ScaleForPixelHeight(&info, px);
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
  const int baseline = pad + int(std::ceil(float(ascent) * first_scale));
  const bool cjk_first = CjkFirst(&info, s);  // TW2 (2026-11-14): fallback fonts
  // The width: the pens' advances.
  float width = 0.0f;
  uint32_t prev = 0;
  const stbtt_fontinfo *prev_font = nullptr;
  for (size_t i = 0; i < s.size();) {
    const uint32_t cp = NextCodepoint(s, i);
    const stbtt_fontinfo *use = FontFor(&info, cp, cjk_first);
    const float scale = stbtt_ScaleForPixelHeight(use, px);
    if (prev && prev_font == use)
      width += scale * float(stbtt_GetCodepointKernAdvance(use, int(prev), int(cp)));
    int advance, lsb;
    stbtt_GetCodepointHMetrics(use, int(cp), &advance, &lsb);
    width += scale * float(advance);
    prev = cp;
    prev_font = use;
  }
  w = int(std::ceil(width)) + 2 * pad;
  h = int(std::ceil(float(ascent - descent) * first_scale)) + 2 * pad;
  coverage.assign(size_t(w) * h, 0);
  float pen = float(pad);
  prev = 0;
  prev_font = nullptr;
  for (size_t i = 0; i < s.size();) {
    const uint32_t cp = NextCodepoint(s, i);
    const stbtt_fontinfo *use = FontFor(&info, cp, cjk_first);
    const float scale = stbtt_ScaleForPixelHeight(use, px);
    if (prev && prev_font == use)
      pen += scale * float(stbtt_GetCodepointKernAdvance(use, int(prev), int(cp)));
    int advance, lsb;
    stbtt_GetCodepointHMetrics(use, int(cp), &advance, &lsb);
    int gw, gh, xo, yo;
    unsigned char *bitmap =
        stbtt_GetCodepointBitmapSubpixel(use, scale, scale, pen - std::floor(pen), 0.0f, int(cp), &gw, &gh, &xo, &yo);
    if (bitmap) {
      const int gx = int(std::floor(pen)) + xo, gy = baseline + yo;
      for (int by = 0; by < gh; by++) {
        const int py = gy + by;
        if (py < 0 || py >= h)
          continue;
        for (int bx = 0; bx < gw; bx++) {
          const int qx = gx + bx;
          if (qx < 0 || qx >= w)
            continue;
          uint8_t &d = coverage[size_t(py) * w + qx];
          d = std::max(d, bitmap[by * gw + bx]);
        }
      }
      stbtt_FreeBitmap(bitmap, nullptr);
    }
    pen += scale * float(advance);
    prev = cp;
    prev_font = use;
  }
  return true;
}

}  // namespace renut::nxs

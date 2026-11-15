// tw2de (2026-10-23): popup_compose.h.
#include "renut_engine/gpu/backend/popup_compose.h"

#include <algorithm>
#include <cmath>

#include "renut_engine/gpu/backend/popup_font.h"

namespace renut::gpu::popup {
namespace {

using popup_font::Font;
using popup_font::Glyph;

struct Color {
  uint8_t r, g, b, a;
};

// The look: a console-style toast, a dark capsule with a soft top-to-bottom gradient and a lit rim, a round badge with
// a trophy at its left end, the header in light grey and "<gamerscore>G - <name>" in white (720-line pixels; the 1080
// size is 1.5 times). A generic trophy: no logo.
constexpr Color kBarTop{70, 72, 76, 238}, kBarBottom{16, 17, 19, 238};
constexpr Color kRim{150, 152, 156, 255};
constexpr Color kBadgeTop{250, 250, 250, 255}, kBadgeBottom{176, 178, 182, 255};
constexpr Color kTrophy{46, 48, 52, 255};
constexpr Color kHeaderColor{200, 202, 206, 255};
constexpr Color kNameColor{255, 255, 255, 255};
constexpr int kBadgePad = 6, kTextGap = 12, kRightPad = 26, kLineGap = 1, kVPad = 9, kMinTextWidth = 200,
              kMaxTextWidth = 580;

// The next code point of UTF-8 text (an invalid sequence gives U+FFFD and moves one byte).
uint32_t NextCodePoint(std::string_view s, size_t &i) {
  const uint8_t c = uint8_t(s[i]);
  auto cont = [&](size_t k) { return i + k < s.size() && (uint8_t(s[i + k]) & 0xC0) == 0x80; };
  if (c < 0x80) {
    i += 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && cont(1)) {
    const uint32_t cp = (uint32_t(c & 0x1F) << 6) | (uint8_t(s[i + 1]) & 0x3F);
    i += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
    const uint32_t cp = (uint32_t(c & 0x0F) << 12) | (uint32_t(uint8_t(s[i + 1]) & 0x3F) << 6) | (uint8_t(s[i + 2]) & 0x3F);
    i += 3;
    return cp;
  }
  if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
    i += 4;
    return 0xFFFD;  // outside the font in any case
  }
  i += 1;
  return 0xFFFD;
}

const Glyph *Find(const Font &font, uint32_t cp) {
  const Glyph *end = font.glyphs + font.count;
  const Glyph *g = std::lower_bound(font.glyphs, end, cp, [](const Glyph &a, uint32_t v) { return a.codepoint < v; });
  return g != end && g->codepoint == cp ? g : nullptr;
}

// The glyphs of a UTF-8 string: unknown characters as '?', control characters dropped, no-break space as space.
std::vector<const Glyph *> Shape(const Font &font, std::string_view text) {
  std::vector<const Glyph *> out;
  for (size_t i = 0; i < text.size();) {
    uint32_t cp = NextCodePoint(text, i);
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
      continue;
    const Glyph *g = Find(font, cp);
    out.push_back(g ? g : Find(font, '?'));
  }
  return out;
}

int Width(const std::vector<const Glyph *> &glyphs) {
  int w = 0;
  for (const Glyph *g : glyphs)
    w += g->advance;
  return w;
}

// The longest prefix that fits max_width with an ellipsis after it (the whole line when it fits).
void Fit(const Font &font, std::vector<const Glyph *> &glyphs, int max_width) {
  if (Width(glyphs) <= max_width)
    return;
  const Glyph *ellipsis = Find(font, 0x2026);
  const int room = max_width - ellipsis->advance;
  int w = 0;
  size_t n = 0;
  while (n < glyphs.size() && w + glyphs[n]->advance <= room)
    w += glyphs[n++]->advance;
  while (n > 0 && glyphs[n - 1]->codepoint == ' ')
    --n;
  glyphs.resize(n);
  glyphs.push_back(ellipsis);
}

// Source over destination, straight alpha.
void Blend(uint8_t *px, Color c, uint32_t coverage) {
  const float sa = float(c.a) * float(coverage) / (255.0f * 255.0f);
  const float da = float(px[3]) / 255.0f;
  const float oa = sa + da * (1.0f - sa);
  if (oa <= 0.0f)
    return;
  const uint8_t src[3] = {c.r, c.g, c.b};
  for (int k = 0; k < 3; ++k)
    px[k] = uint8_t(std::lround((float(src[k]) * sa + float(px[k]) * da * (1.0f - sa)) / oa));
  px[3] = uint8_t(std::lround(oa * 255.0f));
}

Color Mix(Color a, Color b, float t) {
  auto m = [&](uint8_t x, uint8_t y) { return uint8_t(std::lround(float(x) + (float(y) - float(x)) * t)); };
  return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

// Paints the pixels a shape covers (4 x 4 samples a pixel for the edges), the colour from a vertical gradient over
// [y0, y1) of the image.
template <typename Inside>
void Paint(Image &img, int x0, int y0, int x1, int y1, Color top, Color bottom, float g0, float g1, Inside inside) {
  for (int y = std::max(y0, 0); y < std::min(y1, int(img.height)); ++y) {
    const Color c = Mix(top, bottom, std::clamp((float(y) + 0.5f - g0) / std::max(g1 - g0, 1.0f), 0.0f, 1.0f));
    for (int x = std::max(x0, 0); x < std::min(x1, int(img.width)); ++x) {
      int n = 0;
      for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx)
          n += inside(float(x) + (float(sx) + 0.5f) / 4.0f, float(y) + (float(sy) + 0.5f) / 4.0f) ? 1 : 0;
      if (n)
        Blend(&img.rgba[(size_t(y) * img.width + x) * 4], c, uint32_t(n * 255 / 16));
    }
  }
}

// A capsule: the rectangle [x0, x1] x [y0, y1] with ends rounded to half its height.
bool InCapsule(float x, float y, float x0, float y0, float x1, float y1) {
  const float r = (y1 - y0) * 0.5f, cy = (y0 + y1) * 0.5f;
  if (y < y0 || y > y1)
    return false;
  const float cx = std::clamp(x, x0 + r, x1 - r);
  return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

// The trophy in a badge of centre (cx, cy) and radius r: a cup, two handles, a stem and a base.
bool InTrophy(float x, float y, float cx, float cy, float r) {
  const float u = (x - cx) / r, v = (y - cy) / r;  // v down
  const float top = -0.50f;
  // The cup: the lower half of an ellipse below its rim, and the rim's band.
  if (v >= top && v <= top + 0.62f) {
    const float ex = u / 0.50f, ey = (v - top) / 0.62f;
    if (ex * ex + ey * ey <= 1.0f)
      return true;
  }
  // The handles: rings at the sides, outside the cup.
  for (float side : {-1.0f, 1.0f}) {
    const float hx = u - side * 0.47f, hy = v - (top + 0.20f);
    const float d = std::sqrt(hx * hx + hy * hy);
    if (std::abs(u) > 0.42f && d >= 0.15f && d <= 0.25f)
      return true;
  }
  if (std::abs(u) <= 0.08f && v >= top + 0.55f && v <= 0.36f)  // the stem
    return true;
  if (std::abs(u) <= 0.17f && v >= 0.27f && v <= 0.36f)        // the stem's foot
    return true;
  return std::abs(u) <= 0.32f && v >= 0.36f && v <= 0.50f;      // the base
}

void DrawText(Image &img, const Font &font, const std::vector<const Glyph *> &glyphs, int x, int baseline, Color c) {
  for (const Glyph *g : glyphs) {
    for (int gy = 0; gy < g->h; ++gy) {
      const int y = baseline + g->y + gy;
      if (y < 0 || y >= int(img.height))
        continue;
      for (int gx = 0; gx < g->w; ++gx) {
        const int px = x + g->x + gx;
        const uint8_t cov = font.coverage[g->offset + size_t(gy) * g->w + gx];
        if (cov && px >= 0 && px < int(img.width))
          Blend(&img.rgba[(size_t(y) * img.width + px) * 4], c, cov);
      }
    }
    x += g->advance;
  }
}

}  // namespace

std::string_view HeaderText(uint32_t language) {
  switch (language) {
    case 3:
      return "Erfolg freigeschaltet";
    case 4:
      return "Succ\xC3\xA8s d\xC3\xA9verrouill\xC3\xA9";  // Succès déverrouillé
    case 5:
      return "Logro desbloqueado";
    case 6:
      return "Obiettivo sbloccato";
    default:
      return "Achievement unlocked";
  }
}

std::string SecondLine(std::string_view name, uint32_t gamerscore) {
  std::string second;
  if (gamerscore)
    second = std::to_string(gamerscore) + "G - ";
  second += name;
  return second;
}

void OwnedFont::Add(uint32_t codepoint, int advance, int x, int y, int w, int h, const uint8_t *bytes) {
  entries.push_back({codepoint, advance, x, y, w, h, coverage.size()});
  if (w > 0 && h > 0)
    coverage.insert(coverage.end(), bytes, bytes + size_t(w) * h);
}

std::vector<uint32_t> CodePointsOf(std::string_view a, std::string_view b) {
  std::vector<uint32_t> out = {'?', 0x2026};
  for (std::string_view t : {a, b}) {
    for (size_t i = 0; i < t.size();)
      out.push_back(NextCodePoint(t, i));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

namespace {
Image ComposeIn(const Font &font, std::string_view header, std::string_view name, uint32_t gamerscore, bool large);
}  // namespace

Image Compose(std::string_view header, std::string_view name, uint32_t gamerscore, bool large) {
  return ComposeIn(large ? popup_font::k1080 : popup_font::k720, header, name, gamerscore, large);
}

Image Compose(std::string_view header, std::string_view name, uint32_t gamerscore, bool large, const OwnedFont &owned) {
  // The baked form: 16-bit code points, 8-bit advances and offsets, 8-bit sizes; ascending code points.
  std::vector<Glyph> glyphs;
  glyphs.reserve(owned.entries.size());
  for (const OwnedFont::Entry &e : owned.entries) {
    if (e.codepoint > 0xFFFF || e.advance < -128 || e.advance > 127 || e.x < -128 || e.x > 127 || e.y < -128 ||
        e.y > 127 || e.w < 0 || e.w > 255 || e.h < 0 || e.h > 255)
      return {};
    glyphs.push_back({uint16_t(e.codepoint), int8_t(e.advance), int8_t(e.x), int8_t(e.y), uint8_t(e.w), uint8_t(e.h),
                      uint32_t(e.offset)});
  }
  std::sort(glyphs.begin(), glyphs.end(), [](const Glyph &a, const Glyph &b) { return a.codepoint < b.codepoint; });
  const Font font{owned.ascent, owned.descent, glyphs.data(), int(glyphs.size()), owned.coverage.data()};
  if (!Find(font, '?') || !Find(font, 0x2026))
    return {};
  return ComposeIn(font, header, name, gamerscore, large);
}

namespace {
Image ComposeIn(const Font &font, std::string_view header, std::string_view name, uint32_t gamerscore, bool large) {
  auto px = [&](int v) { return large ? (v * 3 + 1) / 2 : v; };
  const std::string second = SecondLine(name, gamerscore);
  std::vector<const Glyph *> line1 = Shape(font, header), line2 = Shape(font, second);
  Fit(font, line1, px(kMaxTextWidth));
  Fit(font, line2, px(kMaxTextWidth));
  const int text_w = std::max({Width(line1), Width(line2), px(kMinTextWidth)});
  const int line_h = font.ascent + font.descent;
  const int height = 2 * line_h + px(kLineGap) + 2 * px(kVPad);
  const int badge = height - 2 * px(kBadgePad);  // the badge's diameter
  const int text_x = px(kBadgePad) + badge + px(kTextGap);

  Image img;
  img.width = uint32_t(text_x + text_w + px(kRightPad));
  img.height = uint32_t(height);
  img.rgba.assign(size_t(img.width) * img.height * 4, 0);
  const float w = float(img.width), h = float(img.height), rim = large ? 1.5f : 1.0f;
  // The capsule: its rim, then its body inset by the rim's width.
  Paint(img, 0, 0, int(w), int(h), kRim, Mix(kRim, kBarBottom, 0.7f), 0.0f, h,
        [&](float x, float y) { return InCapsule(x, y, 0.0f, 0.0f, w, h); });
  Paint(img, 0, 0, int(w), int(h), kBarTop, kBarBottom, 0.0f, h,
        [&](float x, float y) { return InCapsule(x, y, rim, rim, w - rim, h - rim); });
  // The badge and its trophy.
  const float r = float(badge) * 0.5f, cx = float(px(kBadgePad)) + r, cy = h * 0.5f;
  Paint(img, 0, 0, int(cx + r + 1), int(h), kBadgeTop, kBadgeBottom, cy - r, cy + r,
        [&](float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; });
  Paint(img, 0, 0, int(cx + r + 1), int(h), kTrophy, kTrophy, 0.0f, h,
        [&](float x, float y) { return InTrophy(x, y, cx, cy, r * 0.78f); });
  const int y1 = px(kVPad) + font.ascent;
  DrawText(img, font, line1, text_x, y1, kHeaderColor);
  DrawText(img, font, line2, text_x, y1 + line_h + px(kLineGap), kNameColor);
  return img;
}
}  // namespace

}  // namespace renut::gpu::popup

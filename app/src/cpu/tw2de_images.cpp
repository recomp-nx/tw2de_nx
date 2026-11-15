// tw2de (2026-10-25): a language pack's translated pictures (lang/<name>.zip, images/) in place of the game's
// textures: tutorials, the title logo, event titles, "Game Over".
//
// The pack's images/images.ini, one line a picture (tools/texture_export.py writes it for the game's textures):
//   tutorial020_us/snap_p02_0.png = F3931147DD1DCA4C DXT1 512x288
// (several hashes separated by commas: the same texture in each language's files, so that the picture is used
// whatever the game's language under the pack, language.ini's base)
// the PNG (in images/), the texture it replaces: XXH3 of the texture's data (its first blocks_wide x blocks_high x
// block bytes, as in the game's .imgb), its format (DXT1, DXT5, A8R8G8B8) and size; the PNG is that size.
// At Renut's upload of a texture with one level, of a format and size some picture has (render/textures.cpp Upload,
// before the data is read), its data is hashed; the picture of that hash is encoded to the texture's format (DXT
// blocks: endpoints along the colours' principal axis, refined once by least squares) and written over the data in
// guest memory, in the texture's layout (Xbox 360 tiling, or rows of its pitch) and byte order (16-bit big-endian
// words; A8R8G8B8 32-bit). The data then no longer has that hash: a texture is replaced once a load. A picture is
// encoded at its first use and kept.
//
// A picture larger than the texture (the translation needed more room: the battle text's letters):
//   system/btl_number01.png = 6B1E0A26E1D5F1E5 DXT5 512x128 640x128 btl_number01
// the larger size and the texture's entry (txbh) in its WPD file. Its header there (the "GTEX" at the entry's byte 64:
// format byte 6, width and height u16 big-endian at 10 and 12) gets the larger size as the file is loaded
// (tw2de_wpd_patch.cpp), so the game makes the texture that size (its UI layouts give pixels: tw2de_layouts.cpp).
// Its data in the file is still the original's, and nothing is written there (the larger size would run into the
// next texture's): the texture is found by the hash of the original size's data, and Renut uploads the picture
// itself instead of reading guest memory (ImagesPatch returns it).
//
// tw2de (2026-10-31): parts. A pack carries only what the translators made, not the game's picture around it:
//   title_logo/vcc0d314fea3028_credits.png = 99D3942FD7FF3F3B DXT5 608x560 at 72,456
// the PNG is a part of the texture (its own size), written at X,Y over the game's own texture data; the rest of the
// texture stays the game's. A texture can have several parts (a line each, the same hashes). For DXT, X, Y and a
// part's size are multiples of 4 (whole blocks: the game's blocks are kept as they are, the part's encoded). A larger
// texture with parts ("... DXT5 512x128 640x128 btl_number01 at 512,0"): the game's texture data (its original
// size) copied to the larger one's top left, the parts over it, the rest transparent; Renut uploads that.
// tools/lang_image_parts.py cuts a translated picture into its parts against the game's texture.
#include "cpu/tw2de_images.h"

#include "cpu/tw2de_lang_pack.h"
#include "cpu/tw2de_wpd_patch.h"

#include <rex/logging.h>

#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de: sdk/src/system/xobject.cpp

namespace tw2de {
namespace {

enum Format : uint32_t { kDxt1 = 18, kDxt5 = 20, kArgb = 6 };  // Renut's (GpuTextureFormat)

struct Part {
  std::string png;
  bool whole = true;              // the whole texture (no "at X,Y")
  uint32_t x = 0, y = 0;          // a part's place (pixels)
  uint32_t w = 0, h = 0;          // its size (the PNG's)
  std::vector<uint8_t> units;     // its blocks (or pixels) in row order, little-endian
};

struct Picture {
  uint32_t format = 0, width = 0, height = 0;  // the texture's (the larger size when resized)
  uint32_t orig_w = 0, orig_h = 0;             // the game's texture's size (its data)
  uint32_t hash_bytes = 0;                      // of the original data hashed
  bool resized = false;
  bool encoded = false, failed = false, built = false;
  std::vector<Part> parts;
  std::vector<uint8_t> units;  // resized: the whole larger texture, built at its first use
};

std::vector<Picture> g_list;                     // the pictures (not moved after ImagesInit)
std::unordered_map<uint64_t, size_t> g_pictures;  // a texture's hash -> its picture in g_list
std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> g_kinds;  // format, width, height, bytes hashed
std::mutex g_mutex;
std::atomic<bool> g_active{false};

uint32_t BlockBytes(uint32_t f) { return f == kDxt1 ? 8 : f == kDxt5 ? 16 : 4; }
uint32_t UnitsWide(uint32_t f, uint32_t w) { return f == kArgb ? w : (w + 3) / 4; }
uint32_t UnitsHigh(uint32_t f, uint32_t h) { return f == kArgb ? h : (h + 3) / 4; }

// ---- DXT encoding ----

uint32_t To565(float r, float g, float b) {
  auto q = [](float v, int bits) {
    const int m = (1 << bits) - 1;
    return uint32_t(std::clamp(int(std::lround(v / 255.0f * m)), 0, m));
  };
  return q(r, 5) << 11 | q(g, 6) << 5 | q(b, 5);
}
void From565(uint32_t c, float out[3]) {
  out[0] = float(((c >> 11) & 31) * 255 / 31);
  out[1] = float(((c >> 5) & 63) * 255 / 63);
  out[2] = float((c & 31) * 255 / 31);
}

// A colour block (8 bytes, little-endian) of 16 RGB pixels; with 'any_transparent' (DXT1 only) the 3-colour mode, the
// pixels of 'transparent' at its transparent index.
void EncodeColour(const float px[16][3], const bool transparent[16], bool any_transparent, uint8_t out[8]) {
  float mean[3] = {0, 0, 0};
  int n = 0;
  for (int i = 0; i < 16; ++i)
    if (!transparent[i]) {
      for (int k = 0; k < 3; ++k)
        mean[k] += px[i][k];
      ++n;
    }
  if (n == 0) {  // all transparent
    const uint8_t b[8] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    std::memcpy(out, b, 8);
    return;
  }
  for (float &m : mean)
    m /= float(n);
  float cov[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    if (transparent[i])
      continue;
    const float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
    cov[0] += d[0] * d[0];
    cov[1] += d[0] * d[1];
    cov[2] += d[0] * d[2];
    cov[3] += d[1] * d[1];
    cov[4] += d[1] * d[2];
    cov[5] += d[2] * d[2];
  }
  float axis[3] = {0.577f, 0.577f, 0.577f};
  for (int it = 0; it < 8; ++it) {
    const float v[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                        cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                        cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-6f)
      break;
    for (int k = 0; k < 3; ++k)
      axis[k] = v[k] / len;
  }
  float lo = 1e30f, hi = -1e30f;
  for (int i = 0; i < 16; ++i) {
    if (transparent[i])
      continue;
    const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
    lo = std::min(lo, t);
    hi = std::max(hi, t);
  }
  float e0[3], e1[3];
  for (int k = 0; k < 3; ++k) {
    e0[k] = std::clamp(mean[k] + axis[k] * hi, 0.0f, 255.0f);
    e1[k] = std::clamp(mean[k] + axis[k] * lo, 0.0f, 255.0f);
  }
  const bool three = any_transparent;  // 3-colour mode: c0 <= c1, index 3 transparent
  uint32_t c0 = 0, c1 = 0, bits = 0;
  for (int pass = 0; pass < 2; ++pass) {
    c0 = To565(e0[0], e0[1], e0[2]);
    c1 = To565(e1[0], e1[1], e1[2]);
    if (three ? c0 > c1 : c0 < c1)
      std::swap(c0, c1);
    if (!three && c0 == c1) {
      bits = 0;
      break;
    }
    float p[4][3];
    From565(c0, p[0]);
    From565(c1, p[1]);
    const int colours = three ? 3 : 4;
    for (int k = 0; k < 3; ++k) {
      if (three) {
        p[2][k] = (p[0][k] + p[1][k]) / 2;
      } else {
        p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
        p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
      }
    }
    const float weight[4] = {1.0f, 0.0f, three ? 0.5f : 2.0f / 3, 1.0f / 3};  // of c0
    bits = 0;
    float a = 0, b = 0, c = 0, x0[3] = {0, 0, 0}, x1[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      if (transparent[i]) {
        best = 3;
      } else {
        float bd = 1e30f;
        for (int j = 0; j < colours; ++j) {
          const float d = (px[i][0] - p[j][0]) * (px[i][0] - p[j][0]) + (px[i][1] - p[j][1]) * (px[i][1] - p[j][1]) +
                          (px[i][2] - p[j][2]) * (px[i][2] - p[j][2]);
          if (d < bd) {
            bd = d;
            best = j;
          }
        }
        const float w = weight[best];
        a += w * w;
        b += w * (1 - w);
        c += (1 - w) * (1 - w);
        for (int k = 0; k < 3; ++k) {
          x0[k] += w * px[i][k];
          x1[k] += (1 - w) * px[i][k];
        }
      }
      bits |= uint32_t(best) << (2 * i);
    }
    if (pass == 1)
      break;
    // Least squares: the endpoints that fit these indices best.
    const float det = a * c - b * b;
    if (std::fabs(det) < 1e-3f)
      break;
    for (int k = 0; k < 3; ++k) {
      e0[k] = std::clamp((c * x0[k] - b * x1[k]) / det, 0.0f, 255.0f);
      e1[k] = std::clamp((a * x1[k] - b * x0[k]) / det, 0.0f, 255.0f);
    }
  }
  out[0] = uint8_t(c0);
  out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1);
  out[3] = uint8_t(c1 >> 8);
  out[4] = uint8_t(bits);
  out[5] = uint8_t(bits >> 8);
  out[6] = uint8_t(bits >> 16);
  out[7] = uint8_t(bits >> 24);
}

void EncodeAlpha(const uint8_t a[16], uint8_t out[8]) {
  int hi = 0, lo = 255;
  for (int i = 0; i < 16; ++i) {
    hi = std::max(hi, int(a[i]));
    lo = std::min(lo, int(a[i]));
  }
  uint64_t bits = 0;
  if (hi != lo) {  // a0 > a1: eight levels
    int pal[8] = {hi, lo};
    for (int k = 1; k < 7; ++k)
      pal[k + 1] = ((7 - k) * hi + k * lo) / 7;
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      for (int k = 1; k < 8; ++k)
        if (std::abs(pal[k] - a[i]) < std::abs(pal[best] - a[i]))
          best = k;
      bits |= uint64_t(best) << (3 * i);
    }
  }
  out[0] = uint8_t(hi);
  out[1] = uint8_t(lo);
  for (int k = 0; k < 6; ++k)
    out[2 + k] = uint8_t(bits >> (8 * k));
}

// The picture as the texture's units (row order, little-endian).
std::vector<uint8_t> Encode(const std::vector<uint8_t> &rgba, uint32_t format, uint32_t w, uint32_t h) {
  const uint32_t uw = UnitsWide(format, w), uh = UnitsHigh(format, h), bs = BlockBytes(format);
  std::vector<uint8_t> out(size_t(uw) * uh * bs);
  if (format == kArgb) {
    for (uint32_t i = 0; i < w * h; ++i) {
      const uint8_t *p = &rgba[size_t(i) * 4];
      const uint8_t bgra[4] = {p[2], p[1], p[0], p[3]};
      std::memcpy(&out[size_t(i) * 4], bgra, 4);
    }
    return out;
  }
  for (uint32_t by = 0; by < uh; ++by)
    for (uint32_t bx = 0; bx < uw; ++bx) {
      float px[16][3];
      uint8_t alpha[16];
      bool transparent[16], any_transparent = false;
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = std::min(bx * 4 + uint32_t(i & 3), w - 1), y = std::min(by * 4 + uint32_t(i >> 2), h - 1);
        const uint8_t *p = &rgba[(size_t(y) * w + x) * 4];
        for (int k = 0; k < 3; ++k)
          px[i][k] = p[k];
        alpha[i] = p[3];
        transparent[i] = format == kDxt1 && p[3] < 128;
        any_transparent |= transparent[i];
      }
      uint8_t *o = &out[(size_t(by) * uw + bx) * bs];
      if (format == kDxt5) {
        EncodeAlpha(alpha, o);
        const bool none[16] = {};
        EncodeColour(px, none, false, o + 8);
      } else {
        EncodeColour(px, transparent, any_transparent, o);
      }
    }
  return out;
}

uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t lb) {  // in units
  const uint32_t aw = (width + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb;
}

bool LoadPart(const Picture &t, Part &p) {
  const LangPack *pack = GetLangPack();
  const std::vector<uint8_t> *png = pack ? pack->Find("images/" + p.png) : nullptr;
  if (!png) {
    REXLOG_ERROR("[images] images/{} is not in the pack", p.png);
    return false;
  }
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  if (!DecodePng(*png, w, h, rgba)) {
    REXLOG_ERROR("[images] images/{} is not a PNG", p.png);
    return false;
  }
  if (p.whole) {
    if (uint32_t(w) != t.width || uint32_t(h) != t.height) {
      REXLOG_ERROR("[images] images/{} is {}x{}, the texture {}x{}", p.png, w, h, t.width, t.height);
      return false;
    }
  } else {
    const bool blocks = t.format == kArgb || (w % 4 == 0 && h % 4 == 0);
    if (!blocks || p.x + uint32_t(w) > t.width || p.y + uint32_t(h) > t.height) {
      REXLOG_ERROR("[images] images/{} ({}x{} at {},{}) does not fit the {}x{} texture{}", p.png, w, h, p.x, p.y,
                   t.width, t.height, blocks ? "" : " in whole 4x4 blocks");
      return false;
    }
  }
  p.w = uint32_t(w);
  p.h = uint32_t(h);
  p.units = Encode(rgba, t.format, p.w, p.h);
  return true;
}

bool Load(Picture &t) {
  for (Part &p : t.parts)
    if (!LoadPart(t, p))
      return false;
  return true;
}

// A part's units written into a grid of units (row order, 'grid_w' units a row); 'put' writes one unit.
template <typename Put>
void PlaceParts(const Picture &t, Put put) {
  const uint32_t bs = BlockBytes(t.format);
  for (const Part &p : t.parts) {
    const uint32_t pw = UnitsWide(t.format, p.w), ph = UnitsHigh(t.format, p.h);
    const uint32_t ax = t.format == kArgb ? p.x : p.x / 4, ay = t.format == kArgb ? p.y : p.y / 4;
    for (uint32_t sy = 0; sy < ph; ++sy)
      for (uint32_t sx = 0; sx < pw; ++sx)
        put(ax + sx, ay + sy, &p.units[(size_t(sy) * pw + sx) * bs]);
  }
}

}  // namespace

void ImagesInit() {
  const LangPack *pack = GetLangPack();
  const std::vector<uint8_t> *ini = pack ? pack->Find("images/images.ini") : nullptr;
  if (!ini)
    return;
  const std::string text(ini->begin(), ini->end());
  size_t lines = 0;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t eq = line.find(" = ");
    if (line.empty() || line[0] == ';' || eq == std::string::npos)
      continue;
    Part part;
    part.png = line.substr(0, eq);
    std::string rest = line.substr(eq + 3);
    // "at X,Y" at the end: a part of the texture (see the top)
    const size_t at = rest.rfind(" at ");
    if (at != std::string::npos) {
      unsigned x = 0, y = 0;
      if (std::sscanf(rest.c_str() + at, " at %u,%u", &x, &y) != 2) {
        REXLOG_ERROR("[images] images.ini: not a part's place: {}", line);
        continue;
      }
      part.whole = false;
      part.x = x;
      part.y = y;
      rest.resize(at);
    }
    // The hashes: one, or several separated by commas (the same texture in each language's files).
    const size_t space = rest.find(' ');
    std::vector<uint64_t> hashes;
    for (size_t a = 0; space != std::string::npos && a < space;) {
      size_t comma = rest.find(',', a);
      if (comma == std::string::npos || comma > space)
        comma = space;
      hashes.push_back(std::strtoull(rest.substr(a, comma - a).c_str(), nullptr, 16));
      a = comma + 1;
    }
    Picture t;
    char format[16] = {}, entry[17] = {};
    unsigned big_w = 0, big_h = 0;
    const int fields = space == std::string::npos ? 0
                                                  : std::sscanf(rest.c_str() + space, " %15s %ux%u %ux%u %16s", format,
                                                                &t.width, &t.height, &big_w, &big_h, entry);
    if (hashes.empty() || (fields != 3 && fields != 6)) {
      REXLOG_ERROR("[images] images.ini: not a picture line: {}", line);
      continue;
    }
    const std::string f = format;
    t.format = f == "DXT1" ? uint32_t(kDxt1) : f == "DXT5" ? uint32_t(kDxt5) : f == "A8R8G8B8" ? uint32_t(kArgb) : 0u;
    if (!t.format || !t.width || !t.height) {
      REXLOG_ERROR("[images] images.ini: unknown format or size: {}", line);
      continue;
    }
    if (!part.whole && t.format != kArgb && (part.x % 4 != 0 || part.y % 4 != 0)) {
      REXLOG_ERROR("[images] images.ini: a part's place is a multiple of 4 (whole blocks): {}", line);
      continue;
    }
    t.orig_w = t.width;
    t.orig_h = t.height;
    t.hash_bytes = UnitsWide(t.format, t.width) * UnitsHigh(t.format, t.height) * BlockBytes(t.format);
    if (fields == 6) {
      if (t.format == kArgb || big_w < t.width || big_h < t.height) {
        REXLOG_ERROR("[images] images.ini: a larger picture is DXT1 or DXT5, and larger: {}", line);
        continue;
      }
      t.width = big_w;
      t.height = big_h;
      t.resized = true;
    }
    ++lines;
    // Another part of a texture already listed (the same first hash): added to it.
    auto known = g_pictures.find(hashes.front());
    if (known != g_pictures.end()) {
      Picture &k = g_list[known->second];
      if (k.format != t.format || k.width != t.width || k.height != t.height || k.resized != t.resized || part.whole ||
          k.parts.front().whole) {
        REXLOG_ERROR("[images] images.ini: another line for the same texture, but not a part of it: {}", line);
        continue;
      }
      k.parts.push_back(std::move(part));
      continue;
    }
    if (t.resized) {
      // The texture's header in its file, as the file is loaded (once a texture, whatever its parts).
      const uint32_t fmt = t.format == kDxt1 ? 0x18 : 0x1A, w = t.orig_w, h = t.orig_h, bw = big_w, bh = big_h;
      const std::string name = entry;
      WpdPatchRegister(name, "txbh", [=](uint8_t *data, uint32_t size) {
        uint8_t *g = size >= 96 ? data + 64 : nullptr;
        if (!g || std::memcmp(g, "GTEX", 4) != 0 || g[6] != fmt || (g[10] << 8 | g[11]) != int(w) ||
            (g[12] << 8 | g[13]) != int(h)) {
          REXLOG_WARN("[images] {}: not the texture header the pack was made for: left alone", name);
          return;
        }
        g[10] = uint8_t(bw >> 8);
        g[11] = uint8_t(bw);
        g[12] = uint8_t(bh >> 8);
        g[13] = uint8_t(bh);
        if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[images] {}: {}x{} -> {}x{}", name, w, h, bw, bh);
      });
    }
    t.parts.push_back(std::move(part));
    g_kinds.insert({t.format, t.width, t.height, t.hash_bytes});
    g_list.push_back(std::move(t));
    for (uint64_t h : hashes)
      g_pictures[h] = g_list.size() - 1;
  }
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[images] {}: {} pictures ({} textures)", pack->name, lines, g_pictures.size());
  g_active.store(!g_list.empty(), std::memory_order_release);
}

bool ImagesActive() { return g_active.load(std::memory_order_acquire); }

const std::vector<uint8_t> *ImagesPatch(uint8_t *data, uint32_t format, uint32_t width, uint32_t height, bool tiled,
                                        uint32_t pitch_texels) {
  if (!ImagesActive())
    return nullptr;
  // Each amount of data the pictures of this format and size hash.
  Picture *found = nullptr;
  for (auto k = g_kinds.lower_bound({format, width, height, 0});
       !found && k != g_kinds.end() && std::get<0>(*k) == format && std::get<1>(*k) == width &&
       std::get<2>(*k) == height;
       ++k) {
    auto it = g_pictures.find(XXH3_64bits(data, std::get<3>(*k)));
    if (it != g_pictures.end()) {
      Picture &p = g_list[it->second];
      if (p.format == format && p.width == width && p.height == height)
        found = &p;
    }
  }
  if (!found)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_mutex);
  Picture &t = *found;
  if (t.failed)
    return nullptr;
  if (!t.encoded) {
    t.encoded = true;
    if (!Load(t)) {
      t.failed = true;
      return nullptr;
    }
  }
  const uint32_t bs = BlockBytes(format);
  const uint32_t lb = bs == 16 ? 4 : bs == 8 ? 3 : 2;
  const std::string &name = t.parts.front().png;
  if (t.resized) {
    if (!t.built) {
      t.built = true;
      const uint32_t uw = UnitsWide(format, width), uh = UnitsHigh(format, height);
      if (t.parts.front().whole) {
        t.units = t.parts.front().units;  // the whole larger picture
      } else {
        // Transparent, then the game's texture (its original size, in its own layout) at the top left, the parts.
        static const uint8_t kClear[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
        const uint8_t *clear = format == kDxt5 ? kClear : kClear + 8;  // DXT5: alpha 0; DXT1: 3-colour, index 3
        t.units.resize(size_t(uw) * uh * bs);
        for (size_t i = 0; i < size_t(uw) * uh; ++i)
          std::memcpy(&t.units[i * bs], clear, bs);
        const uint32_t ow = UnitsWide(format, t.orig_w), oh = UnitsHigh(format, t.orig_h);
        const uint32_t opitch = (ow + 31) & ~31u;  // a linear texture's rows: padded to 32 blocks
        for (uint32_t y = 0; y < oh; ++y)
          for (uint32_t x = 0; x < ow; ++x) {
            const uint8_t *g = data + size_t(tiled ? TiledOffset(x, y, ow, lb) : y * opitch + x) * bs;
            uint8_t *u = &t.units[(size_t(y) * uw + x) * bs];
            for (uint32_t k = 0; k < bs; ++k)
              u[k] = g[k ^ 1];  // 16-bit big-endian words -> little-endian
          }
        PlaceParts(t, [&](uint32_t x, uint32_t y, const uint8_t *u) {
          std::memcpy(&t.units[(size_t(y) * uw + x) * bs], u, bs);
        });
      }
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 16)
      if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[images] {} ({}x{}, uploaded from the picture)", name, width, height);
    return &t.units;
  }
  const uint32_t uw = UnitsWide(format, width);
  const uint32_t pitch = format == kArgb ? pitch_texels : pitch_texels / 4;
  PlaceParts(t, [&](uint32_t x, uint32_t y, const uint8_t *u) {
    uint8_t *o = data + size_t(tiled ? TiledOffset(x, y, uw, lb) : y * pitch + x) * bs;
    if (format == kArgb)
      for (uint32_t k = 0; k < 4; ++k)
        o[k] = u[3 - k];  // BGRA little-endian -> ARGB big-endian
    else
      for (uint32_t k = 0; k < bs; ++k)
        o[k] = u[k ^ 1];  // 16-bit big-endian words
  });
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[images] {} ({}x{})", name, width, height);
  return nullptr;
}

}  // namespace tw2de

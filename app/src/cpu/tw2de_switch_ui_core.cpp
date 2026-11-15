// TW2 (2026-11-14): tw2de_switch_ui_core.h.
//
// The archive (pack0.dzip, little-endian): "DZIP", version 2, the file count, a number, the table's offset (u64) and a
// hash the game does not check (its reader, sub_828D1000: only the magic and the version); the table at the file's end,
// an entry a file: the name's length (u16), the name (with its 0), a time stamp, then three u64: the file's size, its
// data's offset and its packed size. A file's data: one u32 a 64 KB block (the block's offset from the data's start),
// then the blocks, each LZF-compressed alone (the game's sub_828D34B0 reads a block into a 128 KB buffer and always
// decompresses it: a block made of literal runs only, 1/32 larger than its data, is valid and fits).
// A replaced file gets its new data past the archive's end (the tail) and its entry's three numbers changed. A block
// of it that did not change is not held in memory: the tail shows the archive's own bytes of it there (Range::source).
#include "cpu/tw2de_switch_ui_core.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace tw2de::switch_ui {
namespace {

uint16_t Le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t Le64(const uint8_t *p) { return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32; }
uint32_t Be32(const uint8_t *p) {
  return uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
}
void PutLe16(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
void PutLe32(uint8_t *p, uint32_t v) {
  PutLe16(p, v);
  PutLe16(p + 2, v >> 16);
}
void PutLe64(uint8_t *p, uint64_t v) {
  PutLe32(p, uint32_t(v));
  PutLe32(p + 4, uint32_t(v >> 32));
}

constexpr size_t kBlock = 0x10000;
constexpr uint64_t kMaxFile = 64ull << 20;  // no file handled here is near this

// ---- LZF -------------------------------------------------------------------------------------------------------------
// liblzf's format. The bytes written; 0 when the data is not valid or does not fit.
size_t LzfDecompress(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
  const uint8_t *ip = in, *const in_end = in + in_size;
  uint8_t *op = out, *const out_end = out + out_size;
  while (ip < in_end) {
    uint32_t ctrl = *ip++;
    if (ctrl < 32) {
      ctrl++;
      if (size_t(out_end - op) < ctrl || size_t(in_end - ip) < ctrl)
        return 0;
      std::memcpy(op, ip, ctrl);
      op += ctrl;
      ip += ctrl;
    } else {
      uint32_t length = ctrl >> 5;
      if (ip >= in_end)
        return 0;
      if (length == 7) {
        length += *ip++;
        if (ip >= in_end)
          return 0;
      }
      const size_t back = (size_t(ctrl & 0x1F) << 8) + 1 + *ip++;
      length += 2;
      if (size_t(op - out) < back || size_t(out_end - op) < length)
        return 0;
      const uint8_t *ref = op - back;
      do
        *op++ = *ref++;
      while (--length);
    }
  }
  return size_t(op - out);
}

// `size` bytes (a block: at most 64 KB) compressed onto `out`: a greedy match search over the last 8 KB. Never more
// than 1/32 larger than the data (the game's buffer for a block is twice a block).
void LzfCompress(const uint8_t *in, size_t size, std::vector<uint8_t> &out) {
  constexpr size_t kMaxBack = 1 << 13, kMaxLength = 264;
  std::vector<uint32_t> table(1 << 14, 0);  // a position + 1 by the hash of its three bytes
  size_t at = 0, literals = 0;
  auto flush = [&](size_t end) {
    while (literals < end) {
      const size_t run = std::min<size_t>(end - literals, 32);
      out.push_back(uint8_t(run - 1));
      out.insert(out.end(), in + literals, in + literals + run);
      literals += run;
    }
  };
  while (at + 2 < size) {
    const uint32_t three = uint32_t(in[at]) << 16 | uint32_t(in[at + 1]) << 8 | in[at + 2];
    const uint32_t slot = ((three * 2654435761u) >> 18) & ((1u << 14) - 1);
    const size_t ref = table[slot];
    table[slot] = uint32_t(at + 1);
    if (ref && at - (ref - 1) <= kMaxBack && std::memcmp(in + ref - 1, in + at, 3) == 0) {
      const size_t from = ref - 1, limit = std::min(size - at, kMaxLength);
      size_t length = 3;
      while (length < limit && in[from + length] == in[at + length])
        length++;
      flush(at);
      const size_t back = at - from - 1, code = length - 2;
      if (code < 7) {
        out.push_back(uint8_t((back >> 8) | (code << 5)));
      } else {
        out.push_back(uint8_t((back >> 8) | (7 << 5)));
        out.push_back(uint8_t(code - 7));
      }
      out.push_back(uint8_t(back));
      at += length;
      literals = at;
    } else {
      at++;
    }
  }
  flush(size);
}

// ---- the archive -----------------------------------------------------------------------------------------------------
struct Entry {
  std::string name;  // lower case
  uint64_t size = 0, offset = 0, packed = 0;
  uint64_t fields_at = 0;  // the archive offset of its three numbers
};

// The entry's packed bytes and its blocks' offsets in them (one more than its blocks: the last is the packed size);
// with `data`, the file itself too.
bool ReadEntry(const ReadFn &read, const Entry &e, uint64_t data_end, std::vector<uint8_t> &raw,
               std::vector<uint32_t> &offsets, std::vector<uint8_t> *data) {
  if (!e.size || e.size > kMaxFile || e.packed > kMaxFile || e.offset > data_end || e.packed > data_end - e.offset)
    return false;
  const size_t blocks = size_t((e.size + kBlock - 1) / kBlock);
  if (e.packed < blocks * 4)
    return false;
  raw.resize(size_t(e.packed));
  if (!read(e.offset, raw.data(), raw.size()))
    return false;
  offsets.resize(blocks + 1);
  for (size_t b = 0; b < blocks; b++)
    offsets[b] = Le32(raw.data() + b * 4);
  offsets[blocks] = uint32_t(e.packed);
  if (offsets[0] != blocks * 4)
    return false;
  for (size_t b = 0; b < blocks; b++)
    if (offsets[b] >= offsets[b + 1])
      return false;
  if (data) {
    data->resize(size_t(e.size));
    for (size_t b = 0; b < blocks; b++) {
      const size_t want = std::min<size_t>(kBlock, data->size() - b * kBlock);
      if (LzfDecompress(raw.data() + offsets[b], offsets[b + 1] - offsets[b], data->data() + b * kBlock, want) != want)
        return false;
    }
  }
  return true;
}

// What lies past the archive's end in the patched view.
struct Tail {
  uint64_t at = 0;  // the next byte's offset in the view
  std::vector<Range> ranges;

  void Bytes(const std::vector<uint8_t> &bytes) {
    if (bytes.empty())
      return;
    if (ranges.empty() || ranges.back().data.empty() || ranges.back().offset + ranges.back().data.size() != at) {
      Range r;
      r.offset = at;
      ranges.push_back(std::move(r));
    }
    ranges.back().data.insert(ranges.back().data.end(), bytes.begin(), bytes.end());
    at += bytes.size();
  }
  // `size` bytes of the archive itself, from `source`.
  void Copy(uint64_t source, uint64_t size) {
    if (!size)
      return;
    Range r;
    r.offset = at;
    r.source = source;
    r.source_size = size;
    ranges.push_back(std::move(r));
    at += size;
  }
};

// A file's data into the tail; its packed size (it starts at the tail's `at` before the call). A block in `keep` is
// the entry's own, as it is in the archive.
uint64_t PutEntry(Tail &tail, const std::vector<uint8_t> &data, const Entry *e = nullptr,
                  const std::vector<uint32_t> *offsets = nullptr, const std::vector<bool> *keep = nullptr) {
  const size_t blocks = (data.size() + kBlock - 1) / kBlock;
  std::vector<std::vector<uint8_t>> packed(blocks);
  std::vector<uint8_t> table(blocks * 4);
  uint32_t at = uint32_t(blocks * 4);
  auto kept = [&](size_t b) { return e && offsets && keep && b < keep->size() && (*keep)[b] && b + 1 < offsets->size(); };
  for (size_t b = 0; b < blocks; b++) {
    PutLe32(table.data() + b * 4, at);
    if (kept(b)) {
      at += (*offsets)[b + 1] - (*offsets)[b];
    } else {
      LzfCompress(data.data() + b * kBlock, std::min<size_t>(kBlock, data.size() - b * kBlock), packed[b]);
      at += uint32_t(packed[b].size());
    }
  }
  tail.Bytes(table);
  for (size_t b = 0; b < blocks; b++) {
    if (kept(b))
      tail.Copy(e->offset + (*offsets)[b], (*offsets)[b + 1] - (*offsets)[b]);
    else
      tail.Bytes(packed[b]);
  }
  return at;
}

// ---- DXT -------------------------------------------------------------------------------------------------------------
uint16_t To565(const uint8_t *c) { return uint16_t((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | (c[2] >> 3)); }
void From565(uint16_t v, int *c) {
  c[0] = ((v >> 11) & 31) * 255 / 31;
  c[1] = ((v >> 5) & 63) * 255 / 63;
  c[2] = (v & 31) * 255 / 31;
}

// 16 RGBA pixels as a DXT colour block. With `alpha_aware`, pixels all but transparent do not choose its colours.
constexpr int kSeen = 12;
void EncodeColour(const uint8_t px[16][4], bool alpha_aware, uint8_t out[8]) {
  int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0}, count = 0;
  for (int i = 0; i < 16; i++) {
    if (alpha_aware && px[i][3] < kSeen)
      continue;
    count++;
    for (int c = 0; c < 3; c++) {
      lo[c] = std::min<int>(lo[c], px[i][c]);
      hi[c] = std::max<int>(hi[c], px[i][c]);
    }
  }
  if (!count) {
    std::memset(out, 0, 8);
    return;
  }
  // The ends: the pixels furthest apart along the diagonal of the colours' box.
  const int axis[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
  int least = INT_MAX, most = INT_MIN, at_least = 0, at_most = 0;
  for (int i = 0; i < 16; i++) {
    if (alpha_aware && px[i][3] < kSeen)
      continue;
    const int p = px[i][0] * axis[0] + px[i][1] * axis[1] + px[i][2] * axis[2];
    if (p < least)
      least = p, at_least = i;
    if (p > most)
      most = p, at_most = i;
  }
  uint16_t c0 = To565(px[at_most]), c1 = To565(px[at_least]);
  if (c0 < c1)
    std::swap(c0, c1);
  int pal[4][3];
  From565(c0, pal[0]);
  From565(c1, pal[1]);
  for (int c = 0; c < 3; c++) {
    pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
    pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
  }
  uint32_t bits = 0;
  if (c0 != c1) {
    for (int i = 0; i < 16; i++) {
      if (alpha_aware && px[i][3] < kSeen)
        continue;
      int best = 0, best_d = INT_MAX;
      for (int k = 0; k < 4; k++) {
        const int dr = px[i][0] - pal[k][0], dg = px[i][1] - pal[k][1], db = px[i][2] - pal[k][2];
        const int d = dr * dr * 2 + dg * dg * 4 + db * db;
        if (d < best_d)
          best_d = d, best = k;
      }
      bits |= uint32_t(best) << (i * 2);
    }
  }
  PutLe16(out, c0);
  PutLe16(out + 2, c1);
  PutLe32(out + 4, bits);
}

void DecodeColour(const uint8_t in[8], uint8_t px[16][3]) {
  const uint16_t c0 = Le16(in), c1 = Le16(in + 2);
  int pal[4][3];
  From565(c0, pal[0]);
  From565(c1, pal[1]);
  for (int c = 0; c < 3; c++) {
    if (c0 > c1) {
      pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
      pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
    } else {
      pal[2][c] = (pal[0][c] + pal[1][c]) / 2;
      pal[3][c] = 0;
    }
  }
  const uint32_t bits = Le32(in + 4);
  for (int i = 0; i < 16; i++)
    for (int c = 0; c < 3; c++)
      px[i][c] = uint8_t(pal[(bits >> (i * 2)) & 3][c]);
}

// 16 alphas as a DXT5 alpha block.
void EncodeAlpha(const uint8_t a[16], uint8_t out[8]) {
  int lo = 255, hi = 0;
  for (int i = 0; i < 16; i++) {
    lo = std::min<int>(lo, a[i]);
    hi = std::max<int>(hi, a[i]);
  }
  out[0] = uint8_t(hi);
  out[1] = uint8_t(lo);
  std::memset(out + 2, 0, 6);
  if (hi == lo)
    return;
  int pal[8] = {hi, lo};
  for (int k = 2; k < 8; k++)
    pal[k] = ((8 - k) * hi + (k - 1) * lo) / 7;
  uint64_t bits = 0;
  for (int i = 0; i < 16; i++) {
    int best = 0, best_d = INT_MAX;
    for (int k = 0; k < 8; k++) {
      const int d = std::abs(int(a[i]) - pal[k]);
      if (d < best_d)
        best_d = d, best = k;
    }
    bits |= uint64_t(best) << (i * 3);
  }
  for (int k = 0; k < 6; k++)
    out[2 + k] = uint8_t(bits >> (k * 8));
}

// The 4x4 pixels of `image` at (x, y), the image's edge repeated past it.
void Gather(const uint8_t *rgba, int w, int h, int x, int y, uint8_t px[16][4]) {
  for (int j = 0; j < 4; j++)
    for (int i = 0; i < 4; i++)
      std::memcpy(px[j * 4 + i], rgba + (size_t(std::min(y + j, h - 1)) * w + std::min(x + i, w - 1)) * 4, 4);
}

// The Xbox 360's tiled texture layout: the byte offset of block (x, y) in a texture `pitch` blocks wide (a multiple of
// 32) of 2^log2_size bytes a block. Checked against the game's own picture (all 11,520 blocks of the controller's).
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2_size) {
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2_size + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log2_size;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_size)) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

// ---- the button pictures ---------------------------------------------------------------------------------------------
// globals\gui\icons\buttons\[<lang>\<lang>_]pad_<button>_<64x64 | 32x24>.dds, in three forms told by the file's size:
// 1152 bytes: 32x32 DXT5 (the common ones, "64x64" in their names); 4224: 32x32 B8G8R8A8 (a language's "64x64");
// 896: 32x24 DXT5 (a language's small glossy ones). The header (128 bytes) is the game's own, of the first file of the
// form; the pixels are the port's (tex/tw2_pad_icons.png: row 0 the 32x32 style, row 1 the 32x24 one).
constexpr const char *kButtons[14] = {"a_cross",   "b_circle",  "x_square",     "y_triangle",    "back_select",
                                      "start",     "digitup",   "digitdown",    "digitleft",     "digitright",
                                      "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger"};
constexpr const char kButtonsFolder[] = "globals\\gui\\icons\\buttons\\";

int ButtonOf(const std::string &name) {
  if (name.compare(0, sizeof(kButtonsFolder) - 1, kButtonsFolder) != 0)
    return -1;
  const size_t slash = name.rfind('\\');
  const std::string base = name.substr(slash + 1);
  size_t at = base.find("pad_");
  if (at == std::string::npos || (at != 0 && base[at - 1] != '_'))
    return -1;
  std::string rest = base.substr(at + 4);
  for (const char *suffix : {"_64x64.dds", "_32x24.dds"}) {
    const size_t n = std::strlen(suffix);
    if (rest.size() > n && rest.compare(rest.size() - n, n, suffix) == 0) {
      rest.resize(rest.size() - n);
      for (int i = 0; i < 14; i++)
        if (rest == kButtons[i])
          return i;
    }
  }
  return -1;
}

enum Form { kDxt5Big, kBgra, kDxt5Small, kForms };
int FormOf(uint64_t size) { return size == 1152 ? kDxt5Big : size == 4224 ? kBgra : size == 896 ? kDxt5Small : -1; }

bool HeaderIs(const std::vector<uint8_t> &dds, int form) {
  if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || Le32(dds.data() + 16) != 32)
    return false;
  const uint32_t height = Le32(dds.data() + 12), four_cc = Le32(dds.data() + 84);
  if (form == kBgra)
    return height == 32 && four_cc == 0 && Le32(dds.data() + 88) == 32 && Le32(dds.data() + 92) == 0x00FF0000 &&
           Le32(dds.data() + 104) == 0xFF000000;
  return height == (form == kDxt5Small ? 24u : 32u) && four_cc == 0x35545844;  // "DXT5"
}

// The pictures (tex/tw2_pad_icons.png's cells, in this order; the first 14 as kButtons).
enum Art { kA, kB, kX, kY, kMinus, kPlus, kUp, kDown, kLeft, kRight, kL, kR, kZL, kZR, kZLDark, kZRDark, kArts };
constexpr int kIconsW = kArts * 32, kIconsH = 64;

// By position (input_xbox_layout): the game's A is the Switch's B, its X the Switch's Y.
int ArtFor(int art, bool xbox_layout) { return xbox_layout && art < 4 ? art ^ 1 : art; }

// The picture of cell (art, row) of the icons, w x h, as the form's pixel data.
void IconPixels(const Image &icons, int art, int form, std::vector<uint8_t> &out) {
  const int w = 32, h = form == kDxt5Small ? 24 : 32, x0 = art * 32, y0 = form == kDxt5Small ? 32 : 0;
  std::vector<uint8_t> cell(size_t(w) * h * 4);
  for (int y = 0; y < h; y++)
    std::memcpy(cell.data() + size_t(y) * w * 4, icons.rgba.data() + (size_t(y0 + y) * icons.w + x0) * 4, size_t(w) * 4);
  if (form == kBgra) {
    for (size_t i = 0; i < cell.size(); i += 4) {
      const uint8_t p[4] = {cell[i + 2], cell[i + 1], cell[i], cell[i + 3]};
      out.insert(out.end(), p, p + 4);
    }
    return;
  }
  for (int y = 0; y < h; y += 4)
    for (int x = 0; x < w; x += 4) {
      uint8_t px[16][4], alpha[16], block[16];
      Gather(cell.data(), w, h, x, y, px);
      for (int i = 0; i < 16; i++)
        alpha[i] = px[i][3];
      EncodeAlpha(alpha, block);
      EncodeColour(px, true, block + 8);
      out.insert(out.end(), block, block + 16);
    }
}

// ---- the pictures after a GFX file ------------------------------------------------------------------------------------
// The interface's .swf files are Scaleform GFX files with their pictures after them. After the GFX data (its length: the
// u32 at 4) come, big-endian: the pictures' total size, their data, their count and 32 bytes a picture: a name hash, its
// offset in the data, its size, width, height, 0, its format (1 = DXT1, 7 = DXT5) and 1. The data is as the Xbox
// 360's GPU takes it (16-bit words swapped; the large ones tiled).
struct Picture {
  size_t at = 0;  // in the file
  uint32_t size = 0, width = 0, height = 0, format = 0;
};

bool Pictures(const std::vector<uint8_t> &swf, std::vector<Picture> &out) {
  out.clear();
  if (swf.size() < 64 || std::memcmp(swf.data(), "GFX", 3) != 0)
    return false;
  const size_t gfx = Le32(swf.data() + 4);
  if (gfx > swf.size() || swf.size() - gfx < 8)
    return false;
  const size_t total = Be32(swf.data() + gfx), data = gfx + 4;
  if (total > swf.size() - data || swf.size() - data - total < 4)
    return false;
  const size_t list = data + total;
  const uint32_t count = Be32(swf.data() + list);
  for (uint32_t i = 0; i < count && list + 4 + size_t(i + 1) * 32 <= swf.size(); i++) {
    const uint8_t *r = swf.data() + list + 4 + size_t(i) * 32;
    Picture p;
    p.size = Be32(r + 8);
    p.width = Be32(r + 12);
    p.height = Be32(r + 16);
    p.format = Be32(r + 24);
    const uint32_t offset = Be32(r + 4);
    if (offset > total || p.size > total - offset)
      continue;
    p.at = data + offset;
    out.push_back(p);
  }
  return true;
}

void Mark(std::vector<bool> &changed, size_t at, size_t size) {
  for (size_t b = at / kBlock; b <= (at + size - 1) / kBlock && b < changed.size(); b++)
    changed[b] = true;
}

// The game's 32x32 button pictures inside its .swf files (the prompts at the screens' bottoms, the controller screen's
// labels, the panels' tabs: cnormalbutton.swf's and the copies other files carry): each 32x32 DXT5 in 4096 bytes (8
// rows of blocks, 512 bytes a row, not tiled), told by the FNV-1a hash of those bytes.
struct Known {
  uint64_t hash;
  int art;
};
constexpr Known kKnown[] = {
    {0x646F86813A896CC7ull, kA},     {0x230C61FA5B4C68E4ull, kB},     {0x1473BF7916F3C67Eull, kX},
    {0x4CBCC8090DB036DEull, kY},     {0x657599E7C9A6E7C1ull, kZL},    {0x54402121439445BBull, kZR},
    {0x9BE320DBA3E1887Cull, kL},     {0x8A925B864A278A23ull, kR},     {0xDCE3BF0967A99CEBull, kUp},
    {0xB195D2C19EEE7D50ull, kRight}, {0xC89F1FA0BEB1EEDCull, kDown},  {0x4128B19617F19750ull, kLeft},
    {0x6D289A6171CEA2BFull, kMinus}, {0xD373F21E379C8A04ull, kPlus},
    {0x6842A657F63413FDull, kZLDark}, {0xA6910B2EDA968160ull, kZRDark},  // gui.swf's dark LT / RT (the panels' tabs)
};

uint64_t Fnv(const uint8_t *p, size_t size) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; i++)
    h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

// The known button pictures among the file's replaced; how many.
int PatchSwfButtons(std::vector<uint8_t> &swf, const Assets &assets, std::vector<bool> &changed) {
  std::vector<Picture> pictures;
  if (!Pictures(swf, pictures))
    return 0;
  int count = 0;
  std::vector<uint8_t> blocks;
  for (const Picture &p : pictures) {
    if (p.width != 32 || p.height != 32 || p.format != 7 || p.size != 4096)
      continue;
    const uint64_t hash = Fnv(swf.data() + p.at, p.size);
    for (const Known &k : kKnown) {
      if (k.hash != hash)
        continue;
      blocks.clear();
      IconPixels(assets.icons, ArtFor(k.art, assets.xbox_layout), kDxt5Big, blocks);
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8 * 16; x++)
          swf[p.at + size_t(y) * 512 + x] = blocks[size_t(y) * 128 + (x ^ 1)];
      Mark(changed, p.at, p.size);
      count++;
      break;
    }
  }
  return count;
}

// ---- the controller picture ------------------------------------------------------------------------------------------
// globals\gui\ccontrols.swf (the controller configuration screen's parts) has two pictures: the screen's background
// (1280x720) and the controller on a piece of that background (512x358 in 512x360: opaque, drawn at (385, 215) of the
// screen, found by matching its edge to the background). The new one: the same piece of the background with the port's
// controller (tex/tw2_pad.png, with its own alpha) over it.
constexpr int kPadW = 512, kPadH = 358, kPadAtX = 385, kPadAtY = 215;

bool PatchControls(std::vector<uint8_t> &swf, const Image &pad, std::vector<bool> &changed, std::string &note) {
  if (pad.w != kPadW || pad.h != kPadH || pad.rgba.size() != size_t(kPadW) * kPadH * 4) {
    note = "tw2_pad.png is not 512x358";
    return false;
  }
  std::vector<Picture> pictures;
  if (!Pictures(swf, pictures)) {
    note = "ccontrols.swf is not a GFX file with pictures after it";
    return false;
  }
  size_t back_at = 0, pad_at = 0;
  bool back = false, picture = false;
  for (const Picture &p : pictures) {
    if (p.format != 1)
      continue;
    if (p.width == 1280 && p.height == 720 && p.size == 320u * 192 * 8)
      back = true, back_at = p.at;
    else if (p.width == 512 && p.height == 360 && p.size == 128u * 96 * 8)
      picture = true, pad_at = p.at;
  }
  if (!back || !picture) {
    note = "ccontrols.swf: the background or the controller picture is not there";
    return false;
  }
  // The background's blocks under the picture.
  const int bx0 = kPadAtX / 4, by0 = kPadAtY / 4, bx1 = (kPadAtX + kPadW - 1) / 4, by1 = (kPadAtY + kPadH - 1) / 4;
  const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1, rw = bw * 4;
  std::vector<uint8_t> under(size_t(rw) * bh * 4 * 3);
  for (int by = 0; by < bh; by++)
    for (int bx = 0; bx < bw; bx++) {
      const uint8_t *s = swf.data() + back_at + TiledOffset(uint32_t(bx0 + bx), uint32_t(by0 + by), 320, 3);
      const uint8_t block[8] = {s[1], s[0], s[3], s[2], s[5], s[4], s[7], s[6]};
      uint8_t px[16][3];
      DecodeColour(block, px);
      for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
          std::memcpy(under.data() + (size_t(by * 4 + j) * rw + bx * 4 + i) * 3, px[j * 4 + i], 3);
    }
  // The controller over it (the two rows below the picture's 358 repeat its last).
  std::vector<uint8_t> image(size_t(kPadW) * 360 * 4);
  for (int y = 0; y < 360; y++) {
    const int sy = std::min(y, kPadH - 1);
    for (int x = 0; x < kPadW; x++) {
      const uint8_t *b = under.data() + (size_t(kPadAtY + sy - by0 * 4) * rw + (kPadAtX + x - bx0 * 4)) * 3;
      const uint8_t *p = pad.rgba.data() + (size_t(sy) * kPadW + x) * 4;
      uint8_t *o = image.data() + (size_t(y) * kPadW + x) * 4;
      const int a = p[3];
      for (int c = 0; c < 3; c++)
        o[c] = uint8_t((p[c] * a + b[c] * (255 - a) + 127) / 255);
      o[3] = 255;
    }
  }
  for (int by = 0; by < 90; by++)
    for (int bx = 0; bx < 128; bx++) {
      uint8_t px[16][4], block[8];
      Gather(image.data(), kPadW, 360, bx * 4, by * 4, px);
      EncodeColour(px, false, block);
      uint8_t *d = swf.data() + pad_at + TiledOffset(uint32_t(bx), uint32_t(by), 128, 3);
      const uint8_t swapped[8] = {block[1], block[0], block[3], block[2], block[5], block[4], block[7], block[6]};
      std::memcpy(d, swapped, 8);
    }
  Mark(changed, pad_at, 128u * 96 * 8);
  return true;
}

// ---- the options menu ------------------------------------------------------------------------------------------------
// globals\gui\ui_mainmenu.swf, its class gui.PanelMainMenu (ActionScript 2). The game's script (FillOptions) puts the
// menu's rows into MainItems, each [type, id, name, ...]; PanelMainMenu.DataFilled makes a voMenuItem of each (register
// 8), pushes it into an array (register 4) and gives that to the list. The first row of the options is "select storage
// device" (id MM_BTN_SELECT_STORAGE, 56: the first case of the script's switch on an options row, compiledscripts.w2scripts
// at 0x51EE6, before the sound, gameplay, controls, graphics and language rows' 24, 26, 27, 25, 28), which opens the Xbox
// 360's device selector: nothing to choose on the Switch. Before the push of an item, here:
//     if (item.ID == 56) skip the push
// 22 or 23 bytes put in front of "push item 1 array 'push'; call method; pop", and what holds or jumps over them
// made longer: the function's code size, the jumps and ifs across that point, the tag's length and the GFX length
// (the pictures after the GFX data move with it). A chosen row calls the game with its ID, not its place in the list.
constexpr int kStorageItem = 56;

bool PatchMainMenu(std::vector<uint8_t> &swf, std::string &note) {
  if (swf.size() < 64 || std::memcmp(swf.data(), "GFX", 3) != 0) {
    note = "ui_mainmenu.swf is not a GFX file";
    return false;
  }
  const size_t gfx = Le32(swf.data() + 4);
  if (gfx > swf.size()) {
    note = "ui_mainmenu.swf: its length is wrong";
    return false;
  }
  // The tags: after the header (8 bytes, the stage's rectangle, the frame rate and count).
  size_t o = 8 + (5 + 4 * size_t(swf[8] >> 3) + 7) / 8 + 4;
  static const uint8_t kPush[] = {0x96, 0x0B, 0x00, 0x04, 0x08, 0x07, 0x01, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08};
  size_t tag_header = 0, tag_data = 0, tag_size = 0, at = 0;
  int found = 0;
  while (o + 2 <= gfx) {
    const size_t header = o;
    const uint32_t code_size = Le16(swf.data() + o);
    size_t size = code_size & 63;
    o += 2;
    if (size == 63) {
      if (o + 4 > gfx)
        break;
      size = Le32(swf.data() + o);
      o += 4;
    }
    if (o + size > gfx)
      break;
    if ((code_size >> 6) == 59 && size > sizeof(kPush) + 8 && (code_size & 63) == 63) {  // DoInitAction, long form
      for (size_t i = o + 2; i + sizeof(kPush) + 4 <= o + size; i++)
        if (std::memcmp(swf.data() + i, kPush, sizeof(kPush)) == 0 && swf[i + 14] == 0x52 && swf[i + 15] == 0x17 &&
            swf[i + 16] == 0x99) {
          found++;
          tag_header = header, tag_data = o, tag_size = size, at = i;
        }
    }
    if ((code_size >> 6) == 0)
      break;
    o += size;
  }
  if (found != 1) {
    note = "ui_mainmenu.swf: the list's push was found " + std::to_string(found) + " times";
    return false;
  }
  // The tag's constants (its first action): "push" must be the one pushed there; "ID" for the test.
  const size_t code = tag_data + 2, end = tag_data + tag_size;
  if (swf[code] != 0x88) {
    note = "ui_mainmenu.swf: the class has no constants";
    return false;
  }
  int id_index = -1, push_index = -1;
  {
    const size_t pool_end = code + 3 + Le16(swf.data() + code + 1);
    const uint32_t count = Le16(swf.data() + code + 3);
    size_t p = code + 5;
    for (uint32_t i = 0; i < count && p < pool_end; i++) {
      const char *s = reinterpret_cast<const char *>(swf.data() + p);
      const size_t n = strnlen(s, pool_end - p);
      if (n == 2 && std::memcmp(s, "ID", 2) == 0)
        id_index = int(i);
      if (n == 4 && std::memcmp(s, "push", 4) == 0)
        push_index = int(i);
      p += n + 1;
    }
  }
  if (id_index < 0 || push_index != swf[at + 13]) {
    note = "ui_mainmenu.swf: the constants are not the expected ones";
    return false;
  }
  // The test.
  std::vector<uint8_t> test;
  if (id_index < 256) {
    const uint8_t push_id[] = {0x96, 0x04, 0x00, 0x04, 0x08, 0x08, uint8_t(id_index)};  // push register 8, "ID"
    test.assign(push_id, push_id + sizeof(push_id));
  } else {
    const uint8_t push_id[] = {0x96, 0x05, 0x00, 0x04, 0x08, 0x09, uint8_t(id_index), uint8_t(id_index >> 8)};
    test.assign(push_id, push_id + sizeof(push_id));
  }
  const uint8_t rest[] = {0x4E,                                                  // get member
                          0x96, 0x05, 0x00, 0x07, uint8_t(kStorageItem), 0, 0, 0,  // push 56
                          0x49,                                                  // equals
                          0x9D, 0x02, 0x00, 16, 0};                              // if: over the push, the call, the pop
  test.insert(test.end(), rest, rest + sizeof(rest));
  const int grow = int(test.size());
  // What holds the point or jumps across it.
  for (size_t a = code; a < end;) {
    const uint8_t op = swf[a];
    size_t body = a + 1, size = 0;
    if (op >= 0x80) {
      if (a + 3 > end)
        break;
      size = Le16(swf.data() + a + 1);
      body = a + 3;
    }
    if (body + size > end)
      break;
    if ((op == 0x9B || op == 0x8E) && size >= 2) {  // a function: its code follows, its size the record's last word
      const size_t code_size = Le16(swf.data() + body + size - 2), first = body + size;
      if (first <= at && at < first + code_size) {
        if (code_size + grow > 0xFFFF) {
          note = "ui_mainmenu.swf: the function is too long";
          return false;
        }
        PutLe16(swf.data() + body + size - 2, uint32_t(code_size + grow));
      }
    } else if (op == 0x94 || op == 0x8F) {  // with / try: none around the list's loop
      size_t region = 0;
      if (op == 0x94 && size >= 2)
        region = Le16(swf.data() + body);
      else if (size >= 7)
        region = size_t(Le16(swf.data() + body + 1)) + Le16(swf.data() + body + 3) + Le16(swf.data() + body + 5);
      if (body + size <= at && at < body + size + region) {
        note = "ui_mainmenu.swf: the list's loop is inside a with or try block";
        return false;
      }
    } else if ((op == 0x99 || op == 0x9D) && size == 2) {  // jump / if
      const size_t next = body + 2;
      const int offset = int16_t(Le16(swf.data() + body));
      const long target = long(next) + offset;
      int moved = offset;
      if (next <= at && target > long(at))
        moved += grow;
      else if (a >= at && target <= long(at))
        moved -= grow;
      if (moved < -32768 || moved > 32767) {
        note = "ui_mainmenu.swf: a jump is too long";
        return false;
      }
      PutLe16(swf.data() + body, uint32_t(uint16_t(int16_t(moved))));
    }
    a = body + size;
  }
  PutLe32(swf.data() + tag_header + 2, uint32_t(tag_size + grow));
  PutLe32(swf.data() + 4, uint32_t(gfx + grow));
  swf.insert(swf.begin() + at, test.begin(), test.end());
  return true;
}

bool SameNoCase(const uint8_t *text, size_t size, const char *lower) {
  const size_t n = std::strlen(lower);
  if (size < n)
    return false;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = text[i];
    if (c >= 'A' && c <= 'Z')
      c = uint8_t(c + 32);
    if (c != uint8_t(lower[i]))
      return false;
  }
  return true;
}

}  // namespace

bool BuildPack0Patch(const ReadFn &read, uint64_t file_size, const Assets &assets, Result &out) {
  out = Result();
  uint8_t header[32];
  if (file_size < 64 || !read(0, header, sizeof(header)) || std::memcmp(header, "DZIP", 4) != 0 || Le32(header + 4) != 2) {
    out.notes = "not a DZIP 2 archive";
    return false;
  }
  const uint32_t count = Le32(header + 8);
  const uint64_t table_at = Le64(header + 16);
  if (table_at < 32 || table_at >= file_size || file_size - table_at > kMaxFile || count > 1000000) {
    out.notes = "the archive's table is not where it should be";
    return false;
  }
  std::vector<uint8_t> table(size_t(file_size - table_at));
  if (!read(table_at, table.data(), table.size())) {
    out.notes = "the archive's table could not be read";
    return false;
  }
  // The interface's files with something to change: the button pictures' files, and the .swf files that hold button
  // pictures (found by reading every one of globals\gui once, on a PC), the controller picture or the options menu.
  static constexpr const char *kSwf[] = {"cnormalbutton", "gui",        "camountcontroller", "cfilter",
                                         "ui_arenaleaderboards", "ui_character", "ui_dialog", "ui_game_dice",
                                         "ui_tutorial",   "ui_mainmenu", "ccontrols"};
  constexpr size_t kSwfs = sizeof(kSwf) / sizeof(kSwf[0]);
  std::vector<Entry> buttons;
  Entry swf[kSwfs];
  {
    static constexpr char kGui[] = "globals\\gui\\";
    size_t p = 0;
    for (uint32_t i = 0; i < count; i++) {
      if (p + 2 > table.size())
        break;
      const size_t length = Le16(table.data() + p);
      p += 2;
      if (p + length + 32 > table.size())
        break;
      const uint8_t *name = table.data() + p, *numbers = name + length + 8;
      if (SameNoCase(name, length, kGui)) {
        Entry e;
        e.name.assign(reinterpret_cast<const char *>(name), strnlen(reinterpret_cast<const char *>(name), length));
        for (char &c : e.name)
          if (c >= 'A' && c <= 'Z')
            c = char(c + 32);
        e.size = Le64(numbers);
        e.offset = Le64(numbers + 8);
        e.packed = Le64(numbers + 16);
        e.fields_at = table_at + p + length + 8;
        if (e.name.size() > sizeof(kGui) + 3 && e.name.compare(e.name.size() - 4, 4, ".swf") == 0) {
          const std::string base = e.name.substr(sizeof(kGui) - 1, e.name.size() - (sizeof(kGui) - 1) - 4);
          for (size_t k = 0; k < kSwfs; k++)
            if (base == kSwf[k])
              swf[k] = e;
        } else if (ButtonOf(e.name) >= 0 && FormOf(e.size) >= 0) {
          buttons.push_back(std::move(e));
        }
      }
      p += length + 32;
    }
  }
  Tail tail;
  tail.at = file_size;
  std::vector<Range> fields;
  auto replace = [&](const Entry &e, uint64_t size, uint64_t offset, uint64_t packed) {
    Range r;
    r.offset = e.fields_at;
    r.data.resize(24);
    PutLe64(r.data.data(), size);
    PutLe64(r.data.data() + 8, offset);
    PutLe64(r.data.data() + 16, packed);
    fields.push_back(std::move(r));
  };
  std::vector<uint8_t> raw, data;
  std::vector<uint32_t> offsets;
  auto note = [&](const std::string &text) {
    if (!out.notes.empty())
      out.notes += "; ";
    out.notes += text;
  };
  const bool icons = assets.icons.w == kIconsW && assets.icons.h == kIconsH &&
                     assets.icons.rgba.size() == size_t(kIconsW) * kIconsH * 4;
  if (!icons)
    note("tw2_pad_icons.png is not 512x64");

  // The button pictures' files: a picture and form is packed once, every file of it points there.
  if (icons) {
    std::vector<uint8_t> head[kForms];
    bool bad[kForms] = {};
    uint64_t made_at[14][kForms] = {}, made_size[14][kForms] = {};
    for (const Entry &e : buttons) {
      const int form = FormOf(e.size);
      const int art = ArtFor(ButtonOf(e.name), assets.xbox_layout);
      if (head[form].empty() && !bad[form]) {
        if (ReadEntry(read, e, table_at, raw, offsets, &data) && HeaderIs(data, form))
          head[form].assign(data.begin(), data.begin() + 128);
        else
          bad[form] = true, note("button pictures of " + std::to_string(e.size) + " bytes are not the expected DDS");
      }
      if (bad[form])
        continue;
      if (!made_size[art][form]) {
        std::vector<uint8_t> dds = head[form];
        IconPixels(assets.icons, art, form, dds);
        if (dds.size() != e.size) {
          bad[form] = true;
          continue;
        }
        made_at[art][form] = tail.at;
        made_size[art][form] = PutEntry(tail, dds);
      }
      replace(e, e.size, made_at[art][form], made_size[art][form]);
      out.icon_files++;
    }
  }

  // The .swf files.
  for (size_t k = 0; k < kSwfs; k++) {
    const Entry &e = swf[k];
    const std::string name = std::string(kSwf[k]) + ".swf";
    const bool is_menu = name == "ui_mainmenu.swf", is_controls = name == "ccontrols.swf";
    if (!e.size) {
      note("no " + name + " in the archive");
      continue;
    }
    if (!ReadEntry(read, e, table_at, raw, offsets, &data)) {
      note(name + " could not be unpacked");
      continue;
    }
    std::vector<bool> changed((data.size() + kBlock - 1) / kBlock, false);
    bool any = false, resized = false;
    if (icons) {
      const int n = PatchSwfButtons(data, assets, changed);
      out.swf_pictures += n;
      any = n > 0;
    }
    std::string why;
    if (is_controls) {
      if (PatchControls(data, assets.pad, changed, why))
        out.pad = any = true;
      else
        note(why);
    }
    if (is_menu && !assets.storage_item) {
      if (PatchMainMenu(data, why))
        out.menu = any = resized = true;
      else
        note(why);
    }
    if (!any)
      continue;
    std::vector<bool> keep(changed.size());
    for (size_t b = 0; b < keep.size(); b++)
      keep[b] = !changed[b];
    const uint64_t at = tail.at;
    const uint64_t packed = resized ? PutEntry(tail, data) : PutEntry(tail, data, &e, &offsets, &keep);
    replace(e, data.size(), at, packed);
    out.swf_files++;
  }

  std::sort(fields.begin(), fields.end(), [](const Range &a, const Range &b) { return a.offset < b.offset; });
  out.ranges = std::move(fields);
  out.size = tail.at;
  for (Range &r : tail.ranges)
    out.ranges.push_back(std::move(r));
  return !out.ranges.empty();
}

}  // namespace tw2de::switch_ui

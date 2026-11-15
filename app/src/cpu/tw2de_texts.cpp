// tw2de (2026-10-25): a language pack's texts (lang/<name>.zip, texts/**.txt) drawn in place of the game's.
//
// The pack's text files (tools/ztr_to_texts.py writes them from the game's .ztr files), one text a line:
//   $key = text
// Lines starting with ';' are comments. The text is UTF-8: ASCII as is, a character of the pack's glyphs
// (tw2de_glyphs.cpp) as the code it replaces, any other character of Windows-1252 as itself; {XX XX ...} are bytes
// in hex, written as they are (the game's own codes: {F1 40} the A button, {00 01} a NUL inside a text, ...); "@r" is
// the game's new line. Encoded once, at the first use, into the game's bytes: characters 0x80-0xFF as 0x85 X (X =
// code - 0x40 for 0x80-0xBE, 0x80 for 0xBF, code - 0x21 for 0xC0-0xFF), the end two NULs.
//
// How the game reads a text (sub_824E0438, a text object's lookup by key, 82BF645C the text manager):
//   sub_824D1700 -> sub_824E2648: each loaded .ztr's node, sub_824E2410(node, key): a binary search of the node's
//   sorted keys ([node+44], [node+36] of them) gives the text's index; sub_8272D580(node, index, buffer, size) then
//   decompresses it, once without a buffer (returning its length, both NULs counted) and once into the buffer the
//   caller allocated for that length. sub_8272D580 is called from nowhere else.
// Here sub_8272D580 gives the pack's text of the index's key ([[node+44] + 4 * index]) instead, both times; a key
// the pack has no text for is decompressed as before. Only texts of loaded files are asked for, so the pack's text
// of a key is used wherever the game's file with that key is loaded.
#include "cpu/tw2de_texts.h"

#include "cpu/tw2de_glyphs.h"
#include "cpu/tw2de_lang_pack.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de: sdk/src/system/xobject.cpp
REX_EXTERN(__imp__sub_8272D580);  // a loaded .ztr's text: (node, index, buffer, size) -> length

namespace tw2de {
namespace {

std::unordered_map<std::string, std::string> g_texts;  // key -> the game's bytes, both NULs included
std::atomic<bool> g_active{false};

int HexDigit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int Cp1252Code(char32_t c) {
  static const char16_t k80[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                   0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                   0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
  if (c < 0x80 || (c >= 0xA0 && c <= 0xFF))
    return int(c);
  for (int i = 0; i < 32; ++i)
    if (k80[i] && k80[i] == c)
      return 0x80 + i;
  return -1;
}

// One text line's text, encoded; 'unknown' counts the characters the fonts have no place for (written as '?').
std::string Encode(const std::string &s, int &unknown) {
  std::string out;
  out.reserve(s.size() + 2);
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = uint8_t(s[i]);
    if (c == '{') {  // bytes in hex
      const size_t end = s.find('}', i);
      if (end != std::string::npos) {
        int high = -1;
        for (size_t k = i + 1; k < end; ++k) {
          const int d = HexDigit(s[k]);
          if (d < 0)
            continue;
          if (high < 0) {
            high = d;
          } else {
            out.push_back(char(high << 4 | d));
            high = -1;
          }
        }
        i = end + 1;
        continue;
      }
    }
    // A UTF-8 character.
    const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    char32_t v = n == 1 ? c : n == 2 ? (c & 31) : n == 3 ? (c & 15) : (c & 7);
    for (int k = 1; k < n && i + size_t(k) < s.size(); ++k)
      v = (v << 6) | (uint8_t(s[i + size_t(k)]) & 63);
    i += size_t(n);
    int code = GlyphCode(v);
    if (code < 0)
      code = Cp1252Code(v);
    if (code < 0) {
      ++unknown;
      code = '?';
    }
    if (code < 0x80) {
      out.push_back(char(code));
    } else {
      out.push_back(char(0x85));
      out.push_back(char(code <= 0xBE ? code - 0x40 : code == 0xBF ? 0x80 : code - 0x21));
    }
  }
  out.push_back('\0');
  out.push_back('\0');
  return out;
}

}  // namespace

void TextsInit() {
  const LangPack *pack = GetLangPack();
  if (!pack)
    return;
  int files = 0, unknown = 0;
  for (const auto &[path, data] : pack->files) {
    if (path.compare(0, 6, "texts/") != 0 || path.size() < 4 || path.compare(path.size() - 4, 4, ".txt") != 0)
      continue;
    ++files;
    size_t pos = 0;
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
      pos = 3;  // a UTF-8 BOM
    const char *text = reinterpret_cast<const char *>(data.data());
    while (pos < data.size()) {
      size_t end = pos;
      while (end < data.size() && text[end] != '\n')
        ++end;
      std::string line(text + pos, end - pos);
      pos = end + 1;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty() || line[0] == ';')
        continue;
      const size_t eq = line.find(" = ");
      if (eq == std::string::npos) {
        if (line.size() >= 2 && line.compare(line.size() - 2, 2, " =") == 0)  // an empty text
          g_texts[line.substr(0, line.size() - 2)] = std::string(2, '\0');
        continue;
      }
      g_texts[line.substr(0, eq)] = Encode(line.substr(eq + 3), unknown);
    }
  }
  REXLOG_INFO("[texts] {}: {} texts from {} files{}", pack->name, g_texts.size(), files,
              unknown ? fmt::format(" ({} characters the fonts do not have, shown as '?')", unknown) : std::string());
  g_active.store(!g_texts.empty(), std::memory_order_release);
}

}  // namespace tw2de

extern "C" REX_FUNC(sub_8272D580) {
  if (tw2de::g_active.load(std::memory_order_acquire)) {
    const uint32_t node = ctx.r3.u32, index = ctx.r4.u32, buffer = ctx.r5.u32, size = ctx.r6.u32;
    auto load32 = [&](uint32_t ea) { return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea)); };
    const uint32_t keys = load32(node + 44);
    const uint32_t key = keys ? load32(keys + 4 * index) : 0;
    if (key) {
      auto it = tw2de::g_texts.find(reinterpret_cast<const char *>(base + key));
      if (it != tw2de::g_texts.end()) {
        const std::string &t = it->second;
        uint32_t length = uint32_t(t.size());
        if (buffer) {
          const uint32_t n = std::min(length, size);
          std::memcpy(base + buffer, t.data(), n);
          if (n < length && n) {  // cut as the game cuts: its last byte a NUL
            base[buffer + n - 1] = 0;
            length = n;
          }
        }
        ctx.r3.u64 = length;
        return;
      }
      if (buffer && REXCVAR_GET(log_diagnostics)) {
        // tw2de (2026-11-01): a text the pack has no text for: its key and the game's own text, once a key.
        __imp__sub_8272D580(ctx, base);
        const std::string name(reinterpret_cast<const char *>(base + key));
        const char *own = reinterpret_cast<const char *>(base + buffer);
        static std::mutex m;
        static std::unordered_set<std::string> seen;
        std::lock_guard lock(m);
        if (seen.insert(name).second)
          REXLOG_INFO("[texts] not in the pack: ${} = {}", name, std::string(own, strnlen(own, std::min<uint32_t>(size, 200))));
        return;
      }
    }
  }
  __imp__sub_8272D580(ctx, base);
}

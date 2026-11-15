// tw2de (2026-10-25): a language pack's changes to the game's UI layouts (lang/<name>.zip, layouts/layouts.ini):
// the .ykd entries of its WPD files (gui/resident/system.x360.xgr: battle text, Game Over, ...), changed as they are
// loaded (tw2de_wpd_patch.cpp). tools/layout_diff.py writes the file from a translation's layouts:
//   [gr_btlfont05]                                 the layout (its entry's name)
//   original = 8C9D3062D363F8F1                    XXH3 of the game's: any other is left alone (logged)
//   sprite 0x2AC0 = btl_number01 590 45 50 37 50 37
//                                                  a sprite record at that offset (of its texture's name): the
//                                                  texture, x, y, width, height of its part of it, drawn width, height
//   colours 0x2AA0 = 808080FF 202020FF 808080FF 202020FF   its four corner colours (bytes as stored)
//   u32 0x0788 = 7                                 a word (little-endian, as the layouts are)
//   float 0x0690 = 144                             a float
// The layouts give a sprite's part of its texture in pixels: a texture made larger by the pack (images.ini, its
// header changed as it is loaded: tw2de_images.cpp) has the parts beyond the original size.
#include "cpu/tw2de_layouts.h"

#include "cpu/tw2de_lang_pack.h"
#include "cpu/tw2de_wpd_patch.h"

#include <rex/logging.h>

#include <xxhash.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace tw2de {
namespace {

struct Edit {
  uint32_t offset = 0;
  std::vector<uint8_t> bytes;
};
struct Layout {
  std::string name;
  uint64_t original = 0;
  std::vector<Edit> edits;
};

void PutLe32(std::vector<uint8_t> &out, uint32_t v) {
  for (int k = 0; k < 4; ++k)
    out.push_back(uint8_t(v >> (8 * k)));
}

std::string Trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
    ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
    --b;
  return s.substr(a, b - a);
}

// One line's edit; false when it is not one.
bool ParseEdit(const std::string &key, const std::string &value, Edit &e) {
  char kind[16] = {};
  unsigned offset = 0;
  if (std::sscanf(key.c_str(), "%15s %x", kind, &offset) != 2)
    return false;
  e.offset = offset;
  const std::string k = kind;
  if (k == "sprite") {
    char texture[17] = {};
    unsigned v[6];
    if (std::sscanf(value.c_str(), "%16s %u %u %u %u %u %u", texture, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 7)
      return false;
    e.bytes.assign(16, 0);
    std::memcpy(e.bytes.data(), texture, std::strlen(texture));
    for (unsigned x : v)
      PutLe32(e.bytes, x);
    return true;
  }
  if (k == "colours") {
    unsigned c[4];
    if (std::sscanf(value.c_str(), "%x %x %x %x", &c[0], &c[1], &c[2], &c[3]) != 4)
      return false;
    for (unsigned x : c)  // as stored: the hex digits in byte order
      for (int s = 24; s >= 0; s -= 8)
        e.bytes.push_back(uint8_t(x >> s));
    return true;
  }
  if (k == "u32") {
    PutLe32(e.bytes, uint32_t(std::strtoul(value.c_str(), nullptr, 0)));
    return true;
  }
  if (k == "float") {
    const float f = std::strtof(value.c_str(), nullptr);
    uint32_t v;
    std::memcpy(&v, &f, 4);
    PutLe32(e.bytes, v);
    return true;
  }
  return false;
}

}  // namespace

void LayoutsInit() {
  const LangPack *pack = GetLangPack();
  const std::vector<uint8_t> *ini = pack ? pack->Find("layouts/layouts.ini") : nullptr;
  if (!ini)
    return;
  std::vector<std::shared_ptr<Layout>> layouts;
  const std::string text(ini->begin(), ini->end());
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos)
      end = text.size();
    const std::string line = Trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line[0] == ';' || line[0] == '#')
      continue;
    if (line.front() == '[' && line.back() == ']') {
      layouts.push_back(std::make_shared<Layout>());
      layouts.back()->name = Trim(line.substr(1, line.size() - 2));
      continue;
    }
    const size_t eq = line.find('=');
    if (layouts.empty() || eq == std::string::npos)
      continue;
    const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
    if (key == "original") {
      layouts.back()->original = std::strtoull(value.c_str(), nullptr, 16);
      continue;
    }
    Edit e;
    if (ParseEdit(key, value, e))
      layouts.back()->edits.push_back(std::move(e));
    else
      REXLOG_ERROR("[layouts] [{}]: not an edit: {}", layouts.back()->name, line);
  }
  for (const auto &l : layouts) {
    WpdPatchRegister(l->name, "ykd", [l](uint8_t *data, uint32_t size) {
      const uint64_t hash = XXH3_64bits(data, size);
      if (l->original && hash != l->original) {
        REXLOG_WARN("[layouts] {}: not the game's layout this pack was made for (hash {:016X}): left alone", l->name,
                    hash);
        return;
      }
      int applied = 0;
      for (const Edit &e : l->edits)
        if (uint64_t(e.offset) + e.bytes.size() <= size) {
          std::memcpy(data + e.offset, e.bytes.data(), e.bytes.size());
          ++applied;
        }
      REXLOG_INFO("[layouts] {}: {} of {} changes", l->name, applied, l->edits.size());
    });
  }
  REXLOG_INFO("[layouts] {}: {} layouts", pack->name, layouts.size());
}

}  // namespace tw2de

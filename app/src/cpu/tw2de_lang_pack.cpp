// tw2de (2026-10-25, tw2de_lang): a language pack is one zip, sdmc:/switch/tw2de_nx/lang/<name>.zip:
//   language.ini            the language's name and the port's own strings (tw2de_lang_pack.h)
//   texts/**.txt            translated game texts (tw2de_texts.cpp)
//   glyphs/glyphs.ini       replaced characters of the game's fonts, with their PNGs (tw2de_glyphs.cpp)
// Read whole into memory at the first use. Entries stored or deflated (any zip tool's default); folders skipped.
#include "cpu/tw2de_lang_pack.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <mutex>

REXCVAR_DEFINE_STRING(tw2de_lang, "", "Game/Language",
                      "Language pack: the name of a zip in sdmc:/switch/tw2de_nx/lang (\"pl\" = lang/pl.zip); empty "
                      "= none")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de {
namespace {

uint32_t U16(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t U32(const uint8_t *p) { return U16(p) | U16(p + 2) << 16; }

// 'only': read just that entry (the others are skipped).
bool ReadZip(const std::string &path, LangPack &pack, const char *only = nullptr) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    REXLOG_WARN("[lang] {}: not found", path);
    return false;
  }
  std::vector<uint8_t> zip;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    zip.resize(size_t(size));
    if (std::fread(zip.data(), 1, zip.size(), f) != zip.size())
      zip.clear();
  }
  std::fclose(f);
  // The end of the central directory: its signature, searched back from the end (a comment can follow it).
  size_t eocd = SIZE_MAX;
  for (size_t i = zip.size() >= 22 ? zip.size() - 22 : 0; zip.size() >= 22; --i) {
    if (U32(&zip[i]) == 0x06054B50u) {
      eocd = i;
      break;
    }
    if (i == 0 || zip.size() - i > 22 + 65535)
      break;
  }
  if (eocd == SIZE_MAX) {
    REXLOG_ERROR("[lang] {}: not a zip", path);
    return false;
  }
  const uint32_t count = U16(&zip[eocd + 10]);
  size_t p = U32(&zip[eocd + 16]);
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 46 > zip.size() || U32(&zip[p]) != 0x02014B50u) {
      REXLOG_ERROR("[lang] {}: damaged central directory", path);
      return false;
    }
    const uint32_t method = U16(&zip[p + 10]), csize = U32(&zip[p + 20]), usize = U32(&zip[p + 24]);
    const uint32_t name_len = U16(&zip[p + 28]), extra_len = U16(&zip[p + 30]), comment_len = U16(&zip[p + 32]);
    const size_t local = U32(&zip[p + 42]);
    std::string name(reinterpret_cast<const char *>(&zip[p + 46]), name_len);
    p += 46 + name_len + extra_len + comment_len;
    for (char &c : name)
      if (c == '\\')
        c = '/';
    if (name.empty() || name.back() == '/' || (only && name != only))
      continue;
    if (local + 30 > zip.size() || U32(&zip[local]) != 0x04034B50u) {
      REXLOG_ERROR("[lang] {}: {}: damaged entry", path, name);
      continue;
    }
    const size_t data = local + 30 + U16(&zip[local + 26]) + U16(&zip[local + 28]);
    if (data + csize > zip.size()) {
      REXLOG_ERROR("[lang] {}: {}: damaged entry", path, name);
      continue;
    }
    std::vector<uint8_t> out(usize);
    if (method == 0 && csize == usize) {
      std::copy(zip.begin() + data, zip.begin() + data + csize, out.begin());
    } else if (method == 8) {
      z_stream z{};
      inflateInit2(&z, -MAX_WBITS);
      z.next_in = &zip[data];
      z.avail_in = csize;
      z.next_out = out.data();
      z.avail_out = usize;
      const int r = inflate(&z, Z_FINISH);
      inflateEnd(&z);
      if (r != Z_STREAM_END || z.total_out != usize) {
        REXLOG_ERROR("[lang] {}: {}: could not be unpacked", path, name);
        continue;
      }
    } else {
      REXLOG_ERROR("[lang] {}: {}: packed with method {} (only stored or deflated are read)", path, name, method);
      continue;
    }
    pack.files[name] = std::move(out);
  }
  if (!only)
    REXLOG_INFO("[lang] {}: {} files", path, pack.files.size());
  return true;
}

std::string Trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
    ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
    --b;
  return s.substr(a, b - a);
}

// language.ini into the pack. "\n" in a value is a new line.
void ReadLanguageIni(LangPack &pack) {
  const std::vector<uint8_t> *ini = pack.Find("language.ini");
  pack.display_name = pack.name;
  if (!ini)
    return;
  std::string text(ini->begin(), ini->end());
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    text.erase(0, 3);
  std::string section;
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
      section = Trim(line.substr(1, line.size() - 2));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = Trim(line.substr(0, eq));
    std::string value = Trim(line.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);  // quoted: keeps spaces at its ends
    for (size_t i = 0; (i = value.find("\\n", i)) != std::string::npos;)
      value.replace(i, 2, "\n");
    if (section == "language") {
      if (key == "name" && !value.empty())
        pack.display_name = value;
      else if (key == "base" && !value.empty())
        pack.base = value;
      else if (key == "credits")
        pack.credits = value;
    } else if (section == "port") {
      pack.port[key] = value;
    }
  }
}

}  // namespace

const LangPack *GetLangPack() {
  static LangPack pack;
  static bool ok = false;
  static std::once_flag once;
  std::call_once(once, [] {
    pack.name = REXCVAR_GET(tw2de_lang);
    if (pack.name.empty())
      return;
    ok = ReadZip("sdmc:/switch/tw2de_nx/lang/" + pack.name + ".zip", pack);
    if (ok) {
      ReadLanguageIni(pack);
      REXLOG_INFO("[lang] {} ({}, over the game's {}){}{}", pack.display_name, pack.name, pack.base,
                  pack.credits.empty() ? "" : ": ", pack.credits);
    }
  });
  return ok ? &pack : nullptr;
}

std::vector<LangPack> ListLangPacks() {
  std::vector<LangPack> out;
  DIR *dir = opendir("sdmc:/switch/tw2de_nx/lang");
  if (!dir)
    return out;
  while (const dirent *e = readdir(dir)) {
    const std::string file = e->d_name;
    if (file.size() <= 4 || file.compare(file.size() - 4, 4, ".zip") != 0)
      continue;
    LangPack pack;
    pack.name = file.substr(0, file.size() - 4);
    if (!ReadZip("sdmc:/switch/tw2de_nx/lang/" + file, pack, "language.ini"))
      continue;
    ReadLanguageIni(pack);
    pack.files.clear();
    out.push_back(std::move(pack));
  }
  closedir(dir);
  std::sort(out.begin(), out.end(), [](const LangPack &a, const LangPack &b) { return a.name < b.name; });
  return out;
}

std::vector<uint8_t> ReadPackFile(const std::string &name, const char *path) {
  LangPack pack;
  if (name.empty() || !ReadZip("sdmc:/switch/tw2de_nx/lang/" + name + ".zip", pack, path))
    return {};
  const std::vector<uint8_t> *file = pack.Find(path);
  return file ? *file : std::vector<uint8_t>{};
}

const char *PortText(const char *key, const char *fallback) {
  const LangPack *pack = GetLangPack();
  if (!pack)
    return fallback;
  auto it = pack->port.find(key);
  return it == pack->port.end() || it->second.empty() ? fallback : it->second.c_str();
}

}  // namespace tw2de

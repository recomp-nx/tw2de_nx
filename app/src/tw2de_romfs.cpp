// tw2de (2026-11-06): tw2de_romfs.h.
#include "tw2de_romfs.h"

#include <rex/logging.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(__SWITCH__)
#include <dirent.h>
#include <switch.h>
#endif

namespace tw2de::romfs {

namespace {
std::once_flag g_once;
std::unordered_map<std::string, std::vector<uint8_t>> g_files;  // relative path -> bytes; read-only after Preload

#if defined(__SWITCH__)
// Every file under romfs:/<dir> (dir "" or ending in '/') into g_files; the bytes read.
size_t ReadDir(const std::string &dir) {
  size_t bytes = 0;
  DIR *d = opendir(("romfs:/" + dir).c_str());
  if (!d)
    return 0;
  while (dirent *e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..")
      continue;
    const std::string relative = dir + name;
    if (e->d_type == DT_DIR) {
      bytes += ReadDir(relative + "/");
      continue;
    }
    FILE *f = std::fopen(("romfs:/" + relative).c_str(), "rb");
    if (!f)
      continue;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? size_t(size) : 0);
    const bool ok = data.empty() || std::fread(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok) {
      REXLOG_WARN("[romfs] {} could not be read", relative);
      continue;
    }
    bytes += data.size();
    g_files.emplace(relative, std::move(data));
  }
  closedir(d);
  return bytes;
}
#endif
}  // namespace

void Preload() {
  std::call_once(g_once, [] {
#if defined(__SWITCH__)
    const Result rc = romfsInit();  // libnx: the running NRO's romfs as "romfs:"
    if (R_FAILED(rc)) {
      REXLOG_WARN("[romfs] the NRO's romfs could not be mounted (0x{:08X}): its files are not available", rc);
      return;
    }
    const size_t bytes = ReadDir("");
    romfsExit();
    REXLOG_INFO("[romfs] {} files ({} KB) read into RAM, the NRO's romfs unmounted", g_files.size(), (bytes + 1023) / 1024);
#endif
  });
}

const std::vector<uint8_t> *Find(const char *relative) {
  Preload();
  if (!relative)
    return nullptr;
  const auto it = g_files.find(relative);
  return it != g_files.end() ? &it->second : nullptr;
}

}  // namespace tw2de::romfs

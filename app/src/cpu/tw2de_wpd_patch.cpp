// tw2de (2026-10-25): tw2de_wpd_patch.h.
//
// The game's resident files (gui/resident/system.x360.xgr: fonts, UI layouts, textures) are WPD lists: "WPD\0", the
// entry count (u32 big-endian), then 32 bytes an entry from byte 16: name[16], offset, size (u32 big-endian, from the
// file's start), type[8]. Every write of file data into guest memory (a read, or an inflate's output) is looked at:
// one that starts with a WPD list naming a registered entry (the list whole in that write) makes that entry watched,
// at the write's address plus its offset; the write that holds a watched entry's last byte (the data arrives in
// order) has the entry's change applied, before the game reads it. tw2de_glyphs.cpp found the fonts' tables this way
// (tw2de_049.log: the whole file in one read).
#include "cpu/tw2de_wpd_patch.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace tw2de {
namespace {

struct Patch {
  std::string name, ext;
  WpdPatchFn fn;
};
struct Watch {
  uint32_t start, end;  // the entry's guest addresses
  size_t patch;
};
std::vector<Patch> g_patches;
std::vector<Watch> g_watches;
std::mutex g_mutex;
std::atomic<bool> g_active{false};

uint32_t Be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool FieldIs(const uint8_t *field, size_t size, const std::string &s) {
  return s.size() <= size && std::memcmp(field, s.data(), s.size()) == 0 && (s.size() == size || field[s.size()] == 0);
}

}  // namespace

void WpdPatchRegister(const std::string &name, const std::string &ext, WpdPatchFn fn) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_patches.push_back({name, ext, std::move(fn)});
  g_active.store(true, std::memory_order_release);
}

bool WpdPatchActive() { return g_active.load(std::memory_order_acquire); }

void WpdPatchNoteWrite(uint32_t va, uint32_t bytes) {
  if (!WpdPatchActive() || !bytes)
    return;
  auto *mem = REX_KERNEL_MEMORY();
  const uint8_t *p = mem->TranslateVirtual<const uint8_t *>(va);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (bytes >= 16 && std::memcmp(p, "WPD\0", 4) == 0) {
    const uint32_t count = Be32(p + 4);
    if (count < 4096 && 16 + 32ull * count <= bytes) {
      for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = p + 16 + 32 * i;
        for (size_t k = 0; k < g_patches.size(); ++k)
          if (FieldIs(e, 16, g_patches[k].name) && FieldIs(e + 24, 8, g_patches[k].ext)) {
            const uint32_t off = Be32(e + 16), size = Be32(e + 20);
            g_watches.push_back({va + off, va + off + size, k});
          }
      }
    }
  }
  for (size_t i = 0; i < g_watches.size();) {
    const Watch w = g_watches[i];
    if (va < w.end && uint64_t(va) + bytes >= w.end) {
      g_patches[w.patch].fn(mem->TranslateVirtual<uint8_t *>(w.start), w.end - w.start);
      g_watches.erase(g_watches.begin() + ptrdiff_t(i));
    } else {
      ++i;
    }
  }
}

}  // namespace tw2de

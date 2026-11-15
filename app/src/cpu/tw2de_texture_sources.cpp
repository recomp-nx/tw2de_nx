// tw2de (2026-10-25, tw2de_diag_texture_sources): where each texture Renut uploads came from in the game's files,
// for replacing textures by their source (button prompts, controller pictures) without hashing texture data.
//
// Three records, kept by guest address (the newest writer of an address wins; older records it overlaps go):
//   - reads: every NtReadFile (the SDK's g_read_observer, sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp): the buffer holds
//     the file's bytes from the read's offset;
//   - inflates: every native inflate call (tw2de_native_zlib.cpp): its output holds a decompressed stream's bytes
//     from the stream's output offset. A stream's compressed data is found, at its first call, in a read's buffer:
//     the stream is then the file's bytes from (read offset + position in the buffer - bytes the stream used before);
//   - textures: at Renut's upload of a texture (render/textures.cpp Upload), its base address is looked up in both.
// Logged once per texture (header address, base address, size, format), in the game log, as
//   [texsrc] tex <header> <w>x<h> fmt <f> base <addr> <- <file> @<offset> [zlib stream @<compressed start>
//            +<decompressed offset>]
// The archive's file list (fileCode|pos:usize:csize:path) gives the entry from the offset (the entry's start) and the
// texture's place in the entry (the decompressed offset). A base address in no record: "<- ?" (copied there by the game
// from elsewhere, or made by it).
#include "cpu/tw2de_texture_sources.h"

#include "cpu/tw2de_wpd_patch.h"  // tw2de (2026-10-25): a language pack's changes

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(tw2de_diag_texture_sources, false, "tw2de",
                    "Logs, once per texture, the game file (and offset, and zlib stream) its data was loaded from: "
                    "for texture replacement by source file")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::kernel::xboxkrnl {
extern std::atomic<void (*)(const std::string &path, uint64_t offset, uint32_t guest_buffer, uint32_t bytes)>
    g_read_observer;
}

namespace {

// Addresses by what they hold: a guest virtual address in a physical heap (0xA0000000 and up) as its physical address
// with bit 32 set (the textures' base addresses are physical; the same memory has several virtual views), any other as
// itself.
using Key = uint64_t;
constexpr Key kPhysical = 1ull << 32;

Key FromVirtual(uint32_t va) {
  const uint32_t pa = REX_KERNEL_MEMORY()->GetPhysicalAddress(va);
  return pa != UINT32_MAX ? kPhysical | pa : Key(va);
}

struct Range {
  Key end = 0;                // one past the last byte
  const std::string *path = nullptr;  // interned file path
  uint64_t file_offset = 0;   // the file's offset of the range's first byte (reads), or of the stream's start (zlib)
  bool zlib = false;
  uint64_t out_offset = 0;    // zlib: the stream's decompressed offset of the range's first byte
};

std::mutex g_mutex;
std::set<std::string> g_paths;         // interned paths (never erased: a few hundred files)
std::map<Key, Range> g_ranges;         // by first address (Key), not overlapping
struct Stream {
  const std::string *path = nullptr;
  uint64_t start = 0;  // the file offset of the stream's first compressed byte
};
std::unordered_map<uint32_t, Stream> g_streams;  // guest z_stream -> its source (found at its first inflate)
std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> g_logged;  // header, base, size, format
uint64_t g_textures_logged = 0, g_textures_unknown = 0;

const std::string *Intern(const std::string &path) { return &*g_paths.insert(path).first; }

void Insert(Key start, const Range &range) {
  if (range.end <= start)
    return;
  // Cut what it overlaps: a record starting before it keeps its part below; one ending after it keeps its part above.
  auto it = g_ranges.lower_bound(start);
  if (it != g_ranges.begin()) {
    auto prev = std::prev(it);
    if (prev->second.end > start) {
      Range above = prev->second;
      const Key old_end = above.end;
      prev->second.end = start;
      if (old_end > range.end) {
        const uint64_t skip = range.end - prev->first;
        above.file_offset += above.zlib ? 0 : skip;
        above.out_offset += above.zlib ? skip : 0;
        g_ranges[range.end] = above;
      }
    }
  }
  it = g_ranges.lower_bound(start);
  while (it != g_ranges.end() && it->first < range.end) {
    if (it->second.end > range.end) {
      Range above = it->second;
      const uint64_t skip = range.end - it->first;
      above.file_offset += above.zlib ? 0 : skip;
      above.out_offset += above.zlib ? skip : 0;
      g_ranges.erase(it);
      g_ranges[range.end] = above;
      break;
    }
    it = g_ranges.erase(it);
  }
  g_ranges[start] = range;
}

const Range *Find(Key address, Key &start) {
  auto it = g_ranges.upper_bound(address);
  if (it == g_ranges.begin())
    return nullptr;
  --it;
  if (address >= it->second.end)
    return nullptr;
  start = it->first;
  return &it->second;
}

void OnRead(const std::string &path, uint64_t offset, uint32_t buffer, uint32_t bytes) {
  tw2de::WpdPatchNoteWrite(buffer, bytes);  // tw2de (2026-10-25): a language pack's changes (tw2de_wpd_patch.cpp)
  if (!tw2de::TextureSourcesOn())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Range r;
  const Key start = FromVirtual(buffer);
  r.end = start + bytes;
  r.path = Intern(path);
  r.file_offset = offset;
  Insert(start, r);
}

}  // namespace

namespace tw2de {

bool TextureSourcesOn() {
  static const bool on = REXCVAR_GET(tw2de_diag_texture_sources);
  return on;
}

void TextureSourcesInit() {
  // tw2de (2026-10-25): the game's file reads are also watched for a language pack's changes (tw2de_wpd_patch.cpp).
  if (TextureSourcesOn() || WpdPatchActive())
    rex::kernel::xboxkrnl::g_read_observer.store(&OnRead, std::memory_order_release);
  if (!TextureSourcesOn())
    return;
  REXLOG_INFO("[texsrc] on: game file reads, zlib streams and Renut's texture uploads are recorded");
}

// tw2de_native_zlib.cpp, after each inflate call: 'used_in' bytes at 'next_in' gave 'made_out' bytes at 'next_out';
// 'total_in' and 'total_out' are the stream's counts before the call.
void TextureSourcesNoteInflate(uint32_t strm, uint32_t next_in, uint32_t used_in, uint32_t total_in, uint32_t next_out,
                               uint32_t made_out, uint32_t total_out) {
  if (!TextureSourcesOn() || !made_out)
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Stream &stream = g_streams[strm];
  if (total_in == 0 || !stream.path) {
    stream = {};
    const Key in = FromVirtual(next_in);
    Key start = 0;
    if (const Range *src = Find(in, start); src && !src->zlib) {
      stream.path = src->path;
      stream.start = src->file_offset + (in - start) - total_in;
    }
  }
  if (!stream.path)
    return;
  Range r;
  const Key out = FromVirtual(next_out);
  r.end = out + made_out;
  r.path = stream.path;
  r.file_offset = stream.start;
  r.zlib = true;
  r.out_offset = total_out;
  Insert(out, r);
}

// tw2de_native_zlib.cpp, at inflateInit / inflateEnd: the stream's source is found again at its next first call.
void TextureSourcesForgetStream(uint32_t strm) {
  if (!TextureSourcesOn())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_streams.erase(strm);
}

// render/textures.cpp Upload: texture header 'texture_va', its base level's data at physical address 'base' ('bytes'
// long).
void TextureSourcesNoteUpload(uint32_t texture_va, uint32_t base, uint32_t bytes, uint32_t width, uint32_t height,
                              uint32_t depth, uint32_t format, uint32_t mip_levels, bool tiled, uint64_t frame) {
  if (!TextureSourcesOn())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_logged.insert({texture_va, base, bytes, format}).second)
    return;
  if (g_logged.size() > 200000)
    return;
  ++g_textures_logged;
  const Key key = kPhysical | (base & 0x1FFFFFFFu);
  Key start = 0;
  const Range *src = Find(key, start);
  const uint64_t in_range = src ? key - start : 0;
  const bool whole = src && key + bytes <= src->end;
  if (!src) {
    ++g_textures_unknown;
    REXLOG_INFO("[texsrc] frame {} tex {:08X} {}x{}x{} fmt {} mips {}{} base {:08X} size {:X} <- ?", frame, texture_va,
                width, height, depth, format, mip_levels, tiled ? " tiled" : "", base, bytes);
  } else if (src->zlib) {
    REXLOG_INFO("[texsrc] frame {} tex {:08X} {}x{}x{} fmt {} mips {}{} base {:08X} size {:X} <- {} zlib stream @{:X} "
                "+{:X}{}",
                frame, texture_va, width, height, depth, format, mip_levels, tiled ? " tiled" : "", base, bytes,
                *src->path, src->file_offset, src->out_offset + in_range, whole ? "" : " (partly)");
  } else {
    REXLOG_INFO("[texsrc] frame {} tex {:08X} {}x{}x{} fmt {} mips {}{} base {:08X} size {:X} <- {} @{:X}{}", frame,
                texture_va, width, height, depth, format, mip_levels, tiled ? " tiled" : "", base, bytes, *src->path,
                src->file_offset + in_range, whole ? "" : " (partly)");
  }
  if (g_textures_logged % 500 == 0)
    REXLOG_INFO("[texsrc] {} textures logged, {} of them from no recorded file data", g_textures_logged,
                g_textures_unknown);
}

}  // namespace tw2de

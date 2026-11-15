// tw2de (2026-10-17): Renut's renderer on plume's deko3d back end (tw2de_renut_deko3d.h, docs/deko3d-renderer.md).
#include "renut_engine/gpu/render/draw.h"  // tw2de (2026-11-05): rtw ([rtwork])
#include "renut_engine/gpu/tw2de_renut_deko3d.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>



#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <zstd.h>

#include "renut_engine/gpu/tw2de_renut_tuning.h"
#include "tw2de_romfs.h"  // tw2de (2026-11-06): the host shaders' DKSH in the NRO

#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>
#include <switch.h>
#include <uam_api.h>
#endif

#include "renut_engine/gpu/nx_shaders/nx_file.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"
#include "renut_engine/gpu/nx_shaders/shader_store.h"
#include "renut_engine/gpu/nx_shaders/front_end_store.h"
#include "renut_engine/gpu/nx_shaders/late_spec.h"
#include "renut_engine/gpu/nx_shaders/xenos_glsl.h"
#include "renut_engine/gpu/nx_shaders/sorted_index.h"
#include "renut_engine/gpu/nx_shaders/thread_alloc.h"
#include "renut_engine/gpu/nx_shaders/variant_list.h"  // tw2de (2026-10-21): the GLSL the console translated
#include "renut_engine/gpu/shaders/shader_cache.h"
REXCVAR_DEFINE_BOOL(tw2de_deko3d_tiled_cache, true, "Game/Enhancements",  // TW2: on (prologue 23.7-23.8 -> 24.9-25.2 fps, GPU 40.4 -> 35.8-36.8 ms a frame, two runs)
                    "TW2 trial (2026-11-12, renut_nx_ultimate_base's sasrt_gpu_tiled_cache): Maxwell's tiled caching "
                    "(binning), which keeps a screen tile's colour and depth on chip; TW2's RGBA16F and 64-bit depth "
                    "passes are bandwidth bound. Same pixels by the hardware's design. Off: as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

bool LoadShaderCacheFile();

// tw2de (2026-11-12): the DKSH key of each shader variant made, once (the session log's "[dkshkey] <name> <key>"), to
// join the DKSH cache's per-shader statistics (GPRs, scratch, texture fetches; FNXF's deko3d_shader_stats.py method)
// with the GPU profile's pixel shaders.
// tw2de (2026-11-12): plume's barriers() calls by caller in the 10 s report ([barriers]; plume_deko3d.h
// Deko3DBarrierDiagTake).
REXCVAR_DEFINE_BOOL(tw2de_diag_barriers, false, "Game/Diagnostics",
                    "Count deko3d barriers by caller, stages, layouts and wait, in the 10 s report ([barriers])");
REXCVAR_DEFINE_BOOL(tw2de_diag_dksh_keys, false, "Game/Diagnostics",
                    "Log the DKSH cache key of every shader variant made, once each ([dkshkey] name key)");
REXCVAR_DEFINE_INT32(tw2de_dksh_cache_reserve_mb, 60, "tw2de",
                     "Switch: the DKSH cache file (deko3d_shaders.bin) is made this long ahead, so compiled shaders "
                     "are written into space the SD card already has (0: grows with each one)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_deko3d_fast_runtime_compiles, true, "tw2de",
                    "Switch: a shader variant the game waits for is compiled with one backend run (a fast pass); the "
                    "next start's shader pre-pass compiles it again in full (tw2de_variants.txt)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(tw2de_deko3d_late_spec, true, "tw2de",
                    "Switch (2026-10-23): the pre-pass keeps each shader's front end, and a shader variant the game "
                    "waits for is compiled from it with its constants filled in (about a quarter of a compile from "
                    "GLSL); off: from its GLSL, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_deko3d_shader_meta, true, "tw2de",
                    "Switch (2026-10-24): a game shader is made from what the device reads from its GLSL, kept in "
                    "shaders/tw2de_shader_meta.bin, and its GLSL is read from the store only for a variant that has to "
                    "be compiled or that no variant list knows; off: every shader's GLSL is read when it is made")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_deko3d_ms_compression, false, "Game/Diagnostics",
                    "deko3d renderer (2026-10-22): multisampled render targets with the GPU's hardware compression. "
                    "Off by default: Renut reads their samples through a plain 2D view, which misreads compressed tiles "
                    "(blocks of effects missing)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_deko3d_image_reserve_chunks, 6, "tw2de",
                     "deko3d renderer (2026-10-22): 64 MB image heap chunks made at start-up, while the heap is in one "
                     "piece (gameplay's textures need about 240 MB; later the heap can be too fragmented for a new "
                     "chunk). More are made as needed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// tw2de (2026-10-24): plume's buffer arena (Deko3DOptions::bufferArenaMB): Renut's large buffers (upload rings,
// memexport, stream cache) from one block taken at start-up, not from the heap the game's small allocations fragment.
REXCVAR_DEFINE_INT32(tw2de_deko3d_buffer_arena_mb, 512, "tw2de",
                     "deko3d renderer: MB reserved at start-up for buffers of 4 MB and up (power-of-two blocks; the "
                     "heap when full). 0: no arena")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// tw2de (2026-10-29): off by default. With MSAA, the GPU's dither spread the coverage of faint alpha-to-coverage
// pixels (the ice and water spray) over a fixed screen pattern: a grid of dots where the console's image (and the
// game's single-sample path, an alpha test at 0.5) has nothing. Seen on the console with MSAA on at 1280x720 and
// 1024x576; this cvar set to false removed it.
REXCVAR_DEFINE_BOOL(tw2de_deko3d_a2c_dither, false, "Game/Diagnostics",
                    "deko3d renderer (2026-10-22): alpha to coverage dithered over neighbouring pixels (the GPU's "
                    "own default; off since 2026-10-29: a grid of dots around the ice with MSAA)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de::renut_gpu::deko3d {

namespace {

// ---- the DKSH cache next to the NRO: "BDKC", version, the compiler's tag, then records (u64 key, u32 size, DKSH)
// uam 1.1.0 with SwitchWakerHD's 8 patches and tw2de's passes (bnx3, 2026-10-18: with the value numbering).
// tw2de (2026-10-20): bnx4 with uam tw2de flag 1024 (the 128-bit bone rows) in the default flags: a cache
// compiled before is another compiler's.
// tw2de (2026-10-21): bnx5, uam with PressureSink (flags 2048 + 8192 in the default 11711).
constexpr char kCacheTag[16] = "uam110-sw8-bnx5";

constexpr uint32_t kDkshMagic = 0x48534B44u;  // "DKSH", the first word of every record's data
// tw2de (2026-10-22): the cache's generation (<path>.gen, a u32), for compiles made better without starting the
// cache over: a cache of an older generation keeps its records, and those the newer uam would compile differently are
// marked as fast passes (compiled again by the next pre-pass, in use until then). Generation 1: uam's second try for
// a shader above 56 registers (PATCHES.md 13): the records above 56.
constexpr uint32_t kCacheGen = 1;
constexpr uint32_t kRescueAbove = 56;

// tw2de (2026-10-21): the cache file through libnx's file system calls (nx_shaders/nx_file.h: FsFile).
using CacheFile = renut::nxs::NxFile;

// tw2de (2026-10-23): a record's place: its DKSH's offset and size (and 4 bytes of padding, so an index item is
// the 24 bytes of a <path>.idx entry: the file is read straight into the array).
struct DkPlace {
  int64_t at;
  uint32_t size;
  uint32_t pad;
};
using DkIndex = renut::nxs::SortedIndex<DkPlace>;
static_assert(sizeof(DkIndex::Item) == 24 && offsetof(DkIndex::Item, value) == 8, "a <path>.idx entry");

struct DkshCache {
  renut::nxs::NxMutex mutex;
  CacheFile file;
  // key -> offset of the DKSH, size. tw2de (2026-10-23): sorted by key, found by binary search (sorted_index.h).
  DkIndex index;
  int64_t end = 0;  // the end of the last whole record: where the next one is written
  bool read_only = false;  // a write failed: no more records
  uint64_t added = 0, hits = 0, misses = 0;
  std::string path;
  // tw2de (2026-10-21): the file is sized tw2de_dksh_cache_reserve_mb ahead (fsFsCreateFile / fsFileSetSize: the
  // space is the card's before the first record, and nothing is written to it): appends go into it instead of growing
  // the file by every record. The end of the records is kept in <path>.end, written only after the records before it
  // are on the card: past it, whatever the file holds is not a record.
  int64_t synced_end = -1;   // what <path>.end says
  uint32_t unsynced = 0;     // records since
  std::vector<uint8_t> pending;  // records not written yet (the pre-pass's batches), from pending_at
  int64_t pending_at = 0;
  // tw2de (2026-10-21): a batch being written (taken out of pending; CacheLookup reads from it meanwhile), the
  // order of the writes, <path>.end kept open, and when the pending batch's first record came.
  std::vector<uint8_t> inflight;
  int64_t inflight_at = 0;
  std::vector<uint8_t> spare;  // the last batch's buffer, cleared: the next one fills it without regrowing
  renut::nxs::NxMutex write_mutex;
  CacheFile end_file;
  uint64_t batch_started = 0;
  // tw2de (2026-10-21): the keys whose record is a fast pass (plume's Deko3DOptions::fastRuntimeCompiles), kept in
  // <path>.fast (their u64s). A later record of the same key, the full pass, replaces it: the walk keeps a key's last.
  std::unordered_set<uint64_t> fast;
  // tw2de (2026-10-22): c.fast changed since <path>.fast was last written (a batched store's upgrade).
  bool fast_dirty = false;
  // tw2de (2026-10-23): the end of the records <path>.idx lists (-1: none on the card).
  int64_t index_end = -1;
};
DkshCache g_cache;

// tw2de (2026-10-21): the pre-pass's records go to the card at 8 MB or 30 s (every 64 records, a flush and a
// reopened .end each time, broke into the other threads' reads).
constexpr size_t kBatchBytes = 8u << 20;
constexpr uint64_t kBatchNs = 30ull * 1000000000ull;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool WriteEnd(DkshCache &c, int64_t end) {
  if (!c.end_file.IsOpen() && !c.end_file.Open(c.path + ".end", int64_t(sizeof(int64_t))))
    return false;
  return c.end_file.Write(0, &end, sizeof(end)) && c.end_file.Flush();
}

int64_t ReadEndFile(const std::string &path) {
  CacheFile f;
  int64_t end = -1;
  if (!f.Open(path + ".end", -1) || !f.Read(0, &end, sizeof(end)))
    end = -1;
  f.Close();
  return end;
}

bool WritePendingLocked(DkshCache &c) {
  if (c.pending.empty())
    return true;
  const bool ok = c.file.Write(c.pending_at, c.pending.data(), c.pending.size());
  c.pending.clear();
  return ok;
}

// The records up to c.end on the card, then <path>.end saying so.
void SyncCacheLocked(DkshCache &c) {
  if (!c.file.IsOpen())
    return;
  std::lock_guard<renut::nxs::NxMutex> card(renut::nxs::CardMutex());
  if (!WritePendingLocked(c) || !c.file.Flush()) {
    REXLOG_WARN("[deko3d] DKSH cache {}: records could not be written; no more are", c.path);
    c.read_only = true;
    return;
  }
  c.unsynced = 0;
  if (c.synced_end == c.end)
    return;
  if (WriteEnd(c, c.end))
    c.synced_end = c.end;
}

// tw2de (2026-10-21): the pending batch out, written without c.mutex (lookups and adds go on meanwhile), in
// order (write_mutex), one card operation at a time (CardMutex). lock holds c.mutex.
void SyncCacheOut(DkshCache &c, std::unique_lock<renut::nxs::NxMutex> &lock) {
  lock.unlock();
  std::lock_guard<renut::nxs::NxMutex> order(c.write_mutex);
  lock.lock();
  if (!c.file.IsOpen() || c.read_only || (c.pending.empty() && c.synced_end == c.end))
    return;
  // The batch moves out (no copy; lookups read it as inflight), the spare buffer in.
  c.inflight.swap(c.pending);
  c.pending.swap(c.spare);
  const int64_t at = c.pending_at, end = c.end;
  c.inflight_at = at;
  c.unsynced = 0;
  lock.unlock();
  bool ok;
  {
    // c.inflight without c.mutex: only this thread (write_mutex) changes it; lookups only read it.
    std::lock_guard<renut::nxs::NxMutex> card(renut::nxs::CardMutex());
    ok = (c.inflight.empty() || c.file.Write(at, c.inflight.data(), c.inflight.size())) && c.file.Flush() &&
         WriteEnd(c, end);
  }
  lock.lock();
  if (ok) {
    c.inflight.clear();
    c.spare.swap(c.inflight);
    c.synced_end = end;
  }
  else {
    REXLOG_WARN("[deko3d] DKSH cache {}: records could not be written; no more are", c.path);
    c.read_only = true;  // the batch stays readable (inflight) for this run
  }
}

// <path>.fast: every key c.fast has (small: rewritten whole).
void WriteFastLocked(DkshCache &c) {
  std::vector<uint64_t> keys(c.fast.begin(), c.fast.end());
  std::lock_guard<renut::nxs::NxMutex> card(renut::nxs::CardMutex());
  CacheFile f;
  if (!f.Open(c.path + ".fast", int64_t(keys.size() * 8)))
    return;
  if (f.SetSize(int64_t(keys.size() * 8)) && (keys.empty() || f.Write(0, keys.data(), keys.size() * 8)) && f.Flush())
    c.fast_dirty = false;
  f.Close();
}

// tw2de (2026-10-23): <path>.idx, the records' places in one small file. The start walked the cache record by
// record to find them, one card read each (25,937 reads, about 8 s with nothing on screen); now one read of this file
// (24 bytes a record) and a walk of only the records after the end it lists (those added since it was written).
// Header: "BDKIDX1\0", the cache's tag, i64 end of the records listed, u32 count, u32 0, u64 FNV-1a of the entries;
// then u64 key, i64 offset of the DKSH, u32 size, u32 0. Written whole after the records it lists are on the card
// (the cache's load and SaveShaderCache); a file that does not check out is ignored (the walk then does it all).
constexpr char kIndexMagic[8] = {'B', 'D', 'K', 'I', 'D', 'X', '1', 0};
constexpr size_t kIndexHead = 8 + 16 + 8 + 4 + 4 + 8, kIndexEntry = 24;

uint64_t Fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i)
    h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

// The records up to c.synced_end in <path>.idx (c.mutex held).
void WriteIndexLocked(DkshCache &c) {
  if (!c.file.IsOpen() || c.synced_end < 0 || c.index_end == c.synced_end)
    return;
  const int64_t end = c.synced_end;
  const std::vector<DkIndex::Item> &items = c.index.Sorted();  // in key order: the next start uses it as it is
  std::vector<uint8_t> buf(kIndexHead + items.size() * kIndexEntry);
  uint32_t count = 0;
  for (const DkIndex::Item &i : items) {
    if (i.value.at + int64_t(i.value.size) > end)
      continue;  // not on the card yet
    DkIndex::Item e{i.key, {i.value.at, i.value.size, 0}};
    std::memcpy(buf.data() + kIndexHead + size_t(count) * kIndexEntry, &e, kIndexEntry);
    ++count;
  }
  buf.resize(kIndexHead + size_t(count) * kIndexEntry);
  const uint64_t sum = Fnv1a(buf.data() + kIndexHead, buf.size() - kIndexHead);
  std::memcpy(buf.data(), kIndexMagic, 8);
  std::memcpy(buf.data() + 8, kCacheTag, 16);
  std::memcpy(buf.data() + 24, &end, 8);
  std::memcpy(buf.data() + 32, &count, 4);
  std::memcpy(buf.data() + 40, &sum, 8);
  std::lock_guard<renut::nxs::NxMutex> card(renut::nxs::CardMutex());
  CacheFile f;
  if (!f.Open(c.path + ".idx", int64_t(buf.size())))
    return;
  if (f.SetSize(int64_t(buf.size())) && f.Write(0, buf.data(), buf.size()) && f.Flush())
    c.index_end = end;
  f.Close();
}

// <path>.idx into c.index when it checks out against the cache (records up to length): the end it lists, else -1.
int64_t ReadIndex(DkshCache &c, int64_t length) {
  CacheFile f;
  if (!f.Open(c.path + ".idx", -1))
    return -1;
  const int64_t size = f.Size();
  std::vector<uint8_t> buf;
  if (size >= int64_t(kIndexHead) && size <= (64 << 20)) {
    buf.resize(size_t(size));
    if (!f.Read(0, buf.data(), buf.size()))
      buf.clear();
  }
  f.Close();
  if (buf.size() < kIndexHead || std::memcmp(buf.data(), kIndexMagic, 8) != 0 ||
      std::memcmp(buf.data() + 8, kCacheTag, 16) != 0)
    return -1;
  int64_t end = 0;
  uint32_t count = 0;
  uint64_t sum = 0;
  std::memcpy(&end, buf.data() + 24, 8);
  std::memcpy(&count, buf.data() + 32, 4);
  std::memcpy(&sum, buf.data() + 40, 8);
  if (end < 24 || end > length || buf.size() != kIndexHead + size_t(count) * kIndexEntry ||
      Fnv1a(buf.data() + kIndexHead, buf.size() - kIndexHead) != sum)
    return -1;
  // The entries straight into the array (written in key order: no sort; an index of v140, in no order, is sorted).
  std::vector<DkIndex::Item> items(count);
  if (count)
    std::memcpy(items.data(), buf.data() + kIndexHead, size_t(count) * kIndexEntry);
  int64_t last_at = -1;
  uint64_t last_key = 0;
  uint32_t last_size = 0;
  for (DkIndex::Item &i : items) {
    const uint64_t key = i.key;
    const int64_t at = i.value.at;
    const uint32_t sz = i.value.size;
    i.value.pad = 0;
    if (at < 24 + 12 || sz < 64 || at + int64_t(sz) > end)
      return -1;
    if (at > last_at) {
      last_at = at;
      last_key = key;
      last_size = sz;
    }
  }
  // The cache's last listed record where the index says (its key, size and DKSH magic): an index of another file of
  // the same compiler is not used.
  if (count) {
    uint8_t rec[16];
    uint64_t key;
    uint32_t sz, magic;
    if (!c.file.Read(last_at - 12, rec, sizeof(rec)))
      return -1;
    std::memcpy(&key, rec, 8);
    std::memcpy(&sz, rec + 8, 4);
    std::memcpy(&magic, rec + 12, 4);
    if (key != last_key || sz != last_size || magic != kDkshMagic || last_at + int64_t(sz) != end)
      return -1;
  }
  c.index.Assign(std::move(items), true);
  return end;
}

// TW2 (2026-11-15): the default flags' cache is deko3d_shaders.bin. Builds up to 0.1.0 named it
// deko3d_shaders_f11711.bin: that file and what it derives take the plain name at the first start (renamed: the
// compiled shaders stay). A deko3d_shaders.bin already there is from before 2026-11-12 (11711's and 77247's records
// in one file) and goes first with all it derives, so none of its parts stays next to the cache that takes the name.
constexpr unsigned kDefaultUamFlags = 11711u;

std::string DefaultCachePath() {
  static constexpr const char *kDerived[] = {".idx", ".end", ".gen", ".fast"};
  const std::string plain = ShaderDataPath("deko3d_shaders.bin").string();
  const std::string flagged = ShaderDataPath(fmt::format("deko3d_shaders_f{}.bin", kDefaultUamFlags)).string();
  std::error_code ec;
  if (std::filesystem::exists(flagged, ec)) {
    CacheFile::Delete(plain);
    for (const char *derived : kDerived)
      CacheFile::Delete(plain + derived);
    std::filesystem::rename(flagged, plain, ec);
    if (ec) {
      REXLOG_WARN("[deko3d] DKSH cache {}: could not be renamed to {} ({}); used under its old name", flagged, plain,
                  ec.message());
      return flagged;
    }
    REXLOG_INFO("[deko3d] DKSH cache {} renamed to {}", flagged, plain);
  }
  // What the old name derives follows (also after a start that stopped between the two). A part the plain name
  // already has is the newer one: the old name's goes.
  for (const char *derived : kDerived) {
    if (!std::filesystem::exists(flagged + derived, ec))
      continue;
    std::error_code moved;
    if (std::filesystem::exists(plain + derived, ec))
      moved = std::make_error_code(std::errc::file_exists);
    else
      std::filesystem::rename(flagged + derived, plain + derived, moved);
    if (moved)
      CacheFile::Delete(flagged + derived);
  }
  return plain;
}

void LoadCacheOnce() {
  DkshCache &c = g_cache;
  const uint64_t t_start = NowNs();
  int64_t index_end = -1;  // tw2de (2026-10-23): <path>.idx's records, then the walk from its end
  size_t from_index = 0, walked = 0;
  // tw2de (2026-10-18): the code uam makes with other flags (tw2de_deko3d_uam_prmt, tw2de_deko3d_uam_gvn off) has a
  // cache of its own, with the flags in its name (deko3d_shaders_f<flags>.bin).
  const unsigned flags = Deko3DUamFlags();
  // TW2 (2026-11-12): 11711 a file of its own (it shared 77247's, whose shaders the pressure sink across blocks may
  // have miscompiled: the G-buffer's red).
  // TW2 (2026-11-15): that file is deko3d_shaders.bin, the default flags' cache with no flags in its name
  // (DefaultCachePath); 77247 has deko3d_shaders_f77247.bin.
  c.path = flags == kDefaultUamFlags ? DefaultCachePath()
                                     : ShaderDataPath(fmt::format("deko3d_shaders_f{}.bin", flags)).string();
  const int64_t reserve = int64_t(std::max(0, int(REXCVAR_GET(tw2de_dksh_cache_reserve_mb)))) << 20;
  constexpr int64_t kHead = 24;
  bool fresh = !c.file.Open(c.path, -1);
  bool regen = false;
  std::unordered_map<uint64_t, uint32_t> regen_gprs;
  if (!fresh) {
    char head[kHead] = {};
    if (!c.file.Read(0, head, sizeof(head)) || std::memcmp(head, "BDKC", 4) != 0 ||
        std::memcmp(head + 8, kCacheTag, sizeof(kCacheTag)) != 0) {
      REXLOG_INFO("[deko3d] DKSH cache {}: another compiler's or unreadable; starting it over", c.path);
      fresh = true;
    }
    else {
      const int64_t file_length = c.file.Size();
      // With <path>.end, the records end there (past it: the reserve, or a batch never synced); without it (a cache
      // from before), at the file's end.
      const int64_t end_file = ReadEndFile(c.path);
      const bool has_end = end_file >= kHead && end_file <= file_length;
      const int64_t length = has_end ? end_file : file_length;
      // Records up to the first that is not whole: its size runs past the end, or its data is not a DKSH (an append
      // the process did not finish). New records follow the good ones. Before, appends went after such a record,
      // where the next start's walk never found them: Ryujinx runs 20-22 compiled the same ~30 shaders on every start.
      // tw2de (2026-10-22): a cache of an older generation: each record's registers read on the way (its first
      // 64 bytes: the DKSH header and the program's), once.
      uint32_t gen = 0;
      {
        CacheFile g;
        if (!g.Open(c.path + ".gen", -1) || !g.Read(0, &gen, sizeof(gen)))
          gen = 0;
        g.Close();
      }
      regen = gen < kCacheGen;
      int64_t at = kHead;
      // tw2de (2026-10-23): a cache of an older generation reads every record's registers: the walk does it all.
      if (!regen && (index_end = ReadIndex(c, length)) >= kHead) {
        at = index_end;
        c.index_end = index_end;
        from_index = c.index.Size();
      }
      for (;;) {
        uint8_t rec[12 + 64];
        uint64_t key;
        uint32_t size, magic;
        const size_t want_bytes = regen && at + int64_t(sizeof(rec)) <= length ? sizeof(rec) : 16;
        if (at + 16 > length || !c.file.Read(at, rec, want_bytes))
          break;
        std::memcpy(&key, rec, 8);
        std::memcpy(&size, rec + 8, 4);
        std::memcpy(&magic, rec + 12, 4);
        if (size < 64 || size > (16u << 20) || at + 12 + int64_t(size) > length || magic != kDkshMagic)
          break;
        c.index.Set(key, {at + 12, size, 0});
        ++walked;
        if (want_bytes == sizeof(rec)) {
          uint32_t programs_off = 0, gprs = 0;
          std::memcpy(&programs_off, rec + 12 + 16, 4);
          if (programs_off + 12 <= 64)
            std::memcpy(&gprs, rec + 12 + programs_off + 8, 4);
          regen_gprs[key] = gprs;  // a later record of the key replaces it, as in the index
        }
        at += 12 + int64_t(size);
      }
      c.end = at;
      if (!has_end && at != file_length)
        REXLOG_WARN("[deko3d] DKSH cache {}: {} bytes after the last whole record ({} of them) dropped", c.path,
                    file_length - at, c.index.Size());
      // The reserve; or, without one, the file cut at the records' end.
      const int64_t want = std::max(reserve, at);
      if (file_length != want && (reserve > file_length || !has_end) && !c.file.SetSize(want))
        REXLOG_WARN("[deko3d] DKSH cache {}: could not be sized to {} MB", c.path, want >> 20);
    }
  }
  if (!fresh) {
    CacheFile f;
    if (f.Open(c.path + ".fast", -1)) {
      const int64_t size = f.Size();
      std::vector<uint64_t> keys(size_t(std::max<int64_t>(size, 0)) / 8);
      if (!keys.empty() && f.Read(0, keys.data(), keys.size() * 8))
        for (uint64_t k : keys)
          if (c.index.Contains(k))
            c.fast.insert(k);
      f.Close();
    }
  }
  size_t regen_marked = 0;
  if (!fresh && regen) {
    for (const auto &[key, gprs] : regen_gprs)
      if (gprs > kRescueAbove && c.fast.insert(key).second)
        regen_marked++;
  }
  if (fresh) {
    c.file.Close();
    c.index.Clear();
    c.fast.clear();
    CacheFile::Delete(c.path + ".idx");
    c.index_end = -1;
    // Made the reserve's size at once (fsFsCreateFile), then its header.
    if (c.file.Open(c.path, std::max<int64_t>(reserve, kHead))) {
      if (c.file.Size() < std::max<int64_t>(reserve, kHead))
        c.file.SetSize(std::max<int64_t>(reserve, kHead));
      char head[kHead] = {'B', 'D', 'K', 'C', 1, 0, 0, 0};
      std::memcpy(head + 8, kCacheTag, sizeof(kCacheTag));
      if (!c.file.Write(0, head, sizeof(head)))
        c.file.Close();
      c.end = kHead;
    }
  }
  if (c.file.IsOpen())
    SyncCacheLocked(c);  // <path>.end for this file from now on
  const uint64_t t_read = NowNs();
  if (c.file.IsOpen() && (walked || c.index_end < 0))
    WriteIndexLocked(c);  // tw2de (2026-10-23): the next start reads it instead of walking
  if (fresh || !c.fast.empty())
    WriteFastLocked(c);  // a fresh cache's has none; an old one's without the keys no longer in it
  if (fresh || regen) {
    CacheFile g;
    if (g.Open(c.path + ".gen", int64_t(sizeof(kCacheGen))) && g.Write(0, &kCacheGen, sizeof(kCacheGen)))
      g.Flush();
    g.Close();
    if (regen && !fresh)
      REXLOG_INFO("[deko3d] DKSH cache {}: generation {}: {} records above {} registers to compile again", c.path,
                  kCacheGen, regen_marked, kRescueAbove);
  }
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[deko3d] DKSH cache {}: {} variants ({} of them fast passes to compile again){}; loaded in {} ms ({} "
              "from {}.idx, {} records read one by one), its index written in {} ms",
              c.path, c.index.Size(), c.fast.size(), c.file.IsOpen() ? "" : " (cannot write it)",
              (t_read - t_start) / 1000000, from_index, c.path, walked, (NowNs() - t_read) / 1000000);
}

// tw2de (2026-10-21): once, by the shader pre-pass or the renderer, whichever comes first.
void LoadCache() {
  static std::once_flag once;
  std::call_once(once, LoadCacheOnce);
}

bool CacheLookup(uint64_t key, std::vector<uint8_t> &dksh, bool *fast = nullptr) {
  DkshCache &c = g_cache;
  std::lock_guard<renut::nxs::NxMutex> lock(c.mutex);
  if (fast)
    *fast = c.fast.count(key) != 0;
  const DkPlace *place = c.index.Find(key);
  if (!place || !c.file.IsOpen()) {
    c.misses++;
    return false;
  }
  dksh.resize(place->size);
  const int64_t at = place->at;
  bool ok;
  const int64_t n = int64_t(dksh.size());
  if (!c.pending.empty() && at >= c.pending_at && at + n <= c.pending_at + int64_t(c.pending.size())) {
    std::memcpy(dksh.data(), c.pending.data() + (at - c.pending_at), dksh.size());  // the batch not written yet
    ok = true;
  }
  else if (!c.inflight.empty() && at >= c.inflight_at && at + n <= c.inflight_at + int64_t(c.inflight.size())) {
    std::memcpy(dksh.data(), c.inflight.data() + (at - c.inflight_at), dksh.size());  // the one being written
    ok = true;
  }
  else {
    std::lock_guard<renut::nxs::NxMutex> card(renut::nxs::CardMutex());
    ok = c.file.Read(at, dksh.data(), dksh.size());
  }
  uint32_t magic = 0;
  if (ok)
    std::memcpy(&magic, dksh.data(), sizeof(magic));
  if (!ok || magic != kDkshMagic) {
    c.misses++;
    c.index.Erase(key);
    return false;
  }
  c.hits++;
  return true;
}

// sync: on the card now with <path>.end (the renderer's compiles); else in batches (the pre-pass).
// fast: a fast pass's DKSH (to be replaced); a store with fast = false replaces a fast pass's record.
void CacheStore(uint64_t key, const std::vector<uint8_t> &dksh, bool sync, bool fast = false, bool defer = false) {
  DkshCache &c = g_cache;
  std::unique_lock<renut::nxs::NxMutex> lock(c.mutex);
  if (!c.file.IsOpen() || c.read_only || dksh.size() < 64)
    return;
  const bool upgrade = !fast && c.fast.count(key) != 0;
  if (c.index.Contains(key) && !upgrade)
    return;
  const uint32_t size = uint32_t(dksh.size());
  if (c.pending.empty())
    c.pending_at = c.end;
  if (!sync && c.pending.capacity() < kBatchBytes + (256u << 10))
    c.pending.reserve(kBatchBytes + (256u << 10));  // the pre-pass's batch, once: then reused
  if (c.unsynced == 0)
    c.batch_started = NowNs();
  const size_t at = c.pending.size();
  c.pending.resize(at + 12 + dksh.size());
  std::memcpy(c.pending.data() + at, &key, sizeof(key));
  std::memcpy(c.pending.data() + at + 8, &size, sizeof(size));
  std::memcpy(c.pending.data() + at + 12, dksh.data(), dksh.size());
  c.index.Set(key, {c.end + 12, size, 0});
  c.end += 12 + int64_t(size);
  c.added++;
  // tw2de (2026-10-20, Peggle 2's report): written to the card (fsFileFlush), not only handed to the file system
  // service: a process closed with HOME lost the last records and compiled them again at every start.
  // tw2de (2026-10-21): the renderer's compiles one by one; the pre-pass's in batches (a batch cut short is
  // compiled again at the next start: <path>.end does not reach it).
  ++c.unsynced;
  if (fast)
    c.fast.insert(key);
  else if (upgrade)
    c.fast.erase(key);
  // tw2de (2026-10-23): defer: the renderer's compiles, written by the card writer thread (KickCardWriter).
  if (!defer && (sync || c.pending.size() >= kBatchBytes || NowNs() - c.batch_started >= kBatchNs))
    SyncCacheOut(c, lock);
  // The list after the record it names is on the card (a fast record the list misses stays: it is valid code).
  if (fast || upgrade)
    c.fast_dirty = true;
  if (!defer && c.fast_dirty && c.synced_end == c.end)
    WriteFastLocked(c);
}

// tw2de (2026-10-23): the card writes after a run-time compile, off the thread that compiled. The game waits for
// a compile on the render thread; the record of its DKSH (a write, a flush and the .end file), the fast list (rewritten
// whole) and the variant list's line (an open, a write, a flush) were made there before the game went on. Now that
// thread only adds them to memory (lookups read the batch from there) and wakes this one, which writes them in order.
// A process closed before it gets to them compiles those variants again at the next start, as a cut-short batch.
#if defined(PLUME_DEKO3D)
// ---- tw2de (2026-10-24): each game shader's metadata (plume::Deko3DShaderMeta), next to the store
// (tw2de_shader_meta.bin): "FXMETA1" and a byte of 0, then records (u32 payload size, payload: u64 shader hash, then
// the metadata). A record is good for a stored shader while its textKey is the shader's dkKey (the FNV-1a 64 of the
// GLSL the store holds now): a new translation's text has another. Made the first time a shader's GLSL is read (the
// session after that one makes it without reading); written by the card writer thread; a record cut short at the
// end (a process closed during the write) is cut off at the next start.
class ShaderMetaFile {
 public:
  void Open(const std::string &path) {
    std::lock_guard<renut::nxs::NxMutex> lock(mutex_);
    path_ = path;
    std::lock_guard<renut::nxs::NxMutex> card(renut::nxs::CardMutex());
    if (!file_.Open(path, 0))
      return;
    const int64_t size = file_.Size();
    std::vector<uint8_t> data(size > 0 ? size_t(size) : 0);
    if (!data.empty() && !file_.Read(0, data.data(), data.size()))
      data.clear();
    size_t at = 0;
    if (data.size() >= sizeof(kMagic) && std::memcmp(data.data(), kMagic, sizeof(kMagic)) == 0) {
      at = sizeof(kMagic);
      while (at + 4 <= data.size()) {
        uint32_t n;
        std::memcpy(&n, data.data() + at, 4);
        if (at + 4 + n > data.size())
          break;
        uint64_t hash = 0;
        plume::Deko3DShaderMeta meta;
        if (!Decode(data.data() + at + 4, n, hash, meta))
          break;
        metas_[hash] = std::move(meta);
        at += 4 + n;
      }
    } else {
      at = 0;  // missing, empty or not one: made again
    }
    if (at == 0) {
      file_.SetSize(0);
      file_.Write(0, kMagic, sizeof(kMagic));
      at = sizeof(kMagic);
    } else if (int64_t(at) != size) {
      file_.SetSize(int64_t(at));  // a record cut short
    }
    end_ = int64_t(at);
    file_.Flush();
    open_ = true;
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[deko3d] shader metadata: {} shaders ({})", metas_.size(), path);
  }

  // The shader's metadata when it is the one for this text (key).
  bool Find(uint64_t hash, uint64_t key, plume::Deko3DShaderMeta &out) {
    std::lock_guard<renut::nxs::NxMutex> lock(mutex_);
    const auto it = metas_.find(hash);
    if (it == metas_.end() || it->second.textKey != key)
      return false;
    out = it->second;
    return true;
  }

  void Add(uint64_t hash, const plume::Deko3DShaderMeta &meta) {
    std::lock_guard<renut::nxs::NxMutex> lock(mutex_);
    if (!open_)
      return;
    auto [it, fresh] = metas_.try_emplace(hash, meta);
    if (!fresh) {
      if (it->second.textKey == meta.textKey)
        return;
      it->second = meta;  // another translation's: the newer record is the one read (the later one wins)
    }
    std::vector<uint8_t> payload;
    Encode(hash, meta, payload);
    const uint32_t n = uint32_t(payload.size());
    const size_t at = pending_.size();
    pending_.resize(at + 4 + n);
    std::memcpy(pending_.data() + at, &n, 4);
    std::memcpy(pending_.data() + at + 4, payload.data(), n);
  }

  // The card writer thread: what was added, at the end of the file.
  void Flush() {
    std::vector<uint8_t> out;
    int64_t at;
    {
      std::lock_guard<renut::nxs::NxMutex> lock(mutex_);
      if (!open_ || pending_.empty())
        return;
      out.swap(pending_);
      at = end_;
      end_ += int64_t(out.size());
    }
    std::lock_guard<renut::nxs::NxMutex> card(renut::nxs::CardMutex());
    file_.Write(at, out.data(), out.size());
    file_.Flush();
  }

  bool HasPending() {
    std::lock_guard<renut::nxs::NxMutex> lock(mutex_);
    return !pending_.empty();
  }

  // tw2de (2026-10-24): the metadata of every stored shader that has none for its current text, from its GLSL
  // (the whole .dat read at once), so even the first session makes its shaders without reading GLSL. At start-up,
  // after the pre-pass (nothing adds shaders then); once: later starts find them all.
  void Prefill() {
    auto &store = renut::nxs::ShaderStore::Get();
    std::vector<const renut::nxs::StoredShader *> missing;
    {
      std::lock_guard<renut::nxs::NxMutex> lock(mutex_);
      if (!open_)
        return;
      store.ForEach([&](const renut::nxs::StoredShader &s) {
        const auto it = metas_.find(s.entry.hash);
        if (it == metas_.end() || it->second.textKey != s.dkKey)
          missing.push_back(&s);
      });
    }
    if (missing.empty())
      return;
    const auto t0 = std::chrono::steady_clock::now();
    const size_t held = store.LoadGlslIntoMemory();
    size_t made = 0, failed = 0;
    std::string glsl;
    plume::Deko3DShaderMeta meta;
    for (const renut::nxs::StoredShader *s : missing) {
      if (store.ReadGlsl(*s, glsl) && plume::Deko3DShaderMetaFromGlsl(glsl, meta) && meta.textKey == s->dkKey) {
        Add(s->entry.hash, meta);
        ++made;
      } else {
        ++failed;
      }
    }
    if (held)
      store.ReleaseGlslMemory();
    Flush();
    REXLOG_INFO("[deko3d] shader metadata: {} made from the store ({} failed) in {:.0f} ms ({} MB of GLSL read at once)",
                made, failed, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                held >> 20);
  }

 private:
  static constexpr char kMagic[8] = {'F', 'X', 'M', 'E', 'T', 'A', '1', 0};

  template <typename T>
  static void Put(std::vector<uint8_t> &out, T v) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
  }
  struct Reader {
    const uint8_t *p;
    size_t left;
    bool ok = true;
    template <typename T>
    T Get() {
      T v{};
      if (left < sizeof(T)) {
        ok = false;
        return v;
      }
      std::memcpy(&v, p, sizeof(T));
      p += sizeof(T);
      left -= sizeof(T);
      return v;
    }
  };

  static void Encode(uint64_t hash, const plume::Deko3DShaderMeta &m, std::vector<uint8_t> &out) {
    Put<uint64_t>(out, hash);
    Put<uint64_t>(out, m.textKey);
    const plume::Deko3DShaderUsage &u = m.usage;
    Put<uint32_t>(out, u.uboMask);
    Put<uint32_t>(out, u.ssboMask);
    Put<uint8_t>(out, u.textures ? 1 : 0);
    Put<uint32_t>(out, u.txqMask);
    Put<uint32_t>(out, u.inputMask);
    Put<uint32_t>(out, u.outputMask);
    Put<uint32_t>(out, u.zeroOutputMask);
    Put<uint32_t>(out, uint32_t(u.texBindings.size()));
    for (const auto &b : u.texBindings) {
      Put<uint16_t>(out, b.textureWord);
      Put<uint16_t>(out, b.samplerWord);
    }
    Put<uint32_t>(out, uint32_t(u.vertexInputs.size()));
    for (uint32_t v : u.vertexInputs)
      Put<uint32_t>(out, v);
    Put<uint32_t>(out, uint32_t(m.specDefaults.size()));
    for (const auto &[id, value] : m.specDefaults) {
      Put<uint32_t>(out, id);
      Put<uint32_t>(out, value);
    }
  }

  static bool Decode(const uint8_t *p, size_t n, uint64_t &hash, plume::Deko3DShaderMeta &m) {
    Reader r{p, n};
    hash = r.Get<uint64_t>();
    m.textKey = r.Get<uint64_t>();
    plume::Deko3DShaderUsage &u = m.usage;
    u.uboMask = r.Get<uint32_t>();
    u.ssboMask = r.Get<uint32_t>();
    u.textures = r.Get<uint8_t>() != 0;
    u.txqMask = r.Get<uint32_t>();
    u.inputMask = r.Get<uint32_t>();
    u.outputMask = r.Get<uint32_t>();
    u.zeroOutputMask = r.Get<uint32_t>();
    const uint32_t tex = r.Get<uint32_t>();
    if (!r.ok || tex > 256)
      return false;
    for (uint32_t i = 0; i < tex; ++i) {
      plume::Deko3DShaderUsage::TexBinding b;
      b.textureWord = r.Get<uint16_t>();
      b.samplerWord = r.Get<uint16_t>();
      u.texBindings.push_back(b);
    }
    const uint32_t vin = r.Get<uint32_t>();
    if (!r.ok || vin > 256)
      return false;
    for (uint32_t i = 0; i < vin; ++i)
      u.vertexInputs.push_back(r.Get<uint32_t>());
    const uint32_t spec = r.Get<uint32_t>();
    if (!r.ok || spec > 1024)
      return false;
    for (uint32_t i = 0; i < spec; ++i) {
      const uint32_t id = r.Get<uint32_t>();
      const uint32_t value = r.Get<uint32_t>();
      m.specDefaults.emplace_back(id, value);
    }
    return r.ok && r.left == 0;
  }

  renut::nxs::NxMutex mutex_;
  std::string path_;
  renut::nxs::NxFile file_;
  int64_t end_ = 0;
  bool open_ = false;
  std::unordered_map<uint64_t, plume::Deko3DShaderMeta> metas_;
  std::vector<uint8_t> pending_;
};
ShaderMetaFile g_shader_meta;
#endif

struct CardWriter {
  std::mutex mutex;
  std::condition_variable wake;
  bool dirty = false;
  bool started = false;
};
CardWriter g_card_writer;

void CardWriterLoop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(g_card_writer.mutex);
      g_card_writer.wake.wait(lock, [] { return g_card_writer.dirty; });
      g_card_writer.dirty = false;
    }
    {
      DkshCache &c = g_cache;
      std::unique_lock<renut::nxs::NxMutex> lock(c.mutex);
      SyncCacheOut(c, lock);
      if (c.fast_dirty && c.synced_end == c.end)
        WriteFastLocked(c);
    }
    renut::nxs::VariantRecorder::Get().Flush();
#if defined(PLUME_DEKO3D)
    g_shader_meta.Flush();  // tw2de (2026-10-24)
#endif
  }
}

void KickCardWriter() {
  {
    std::lock_guard<std::mutex> lock(g_card_writer.mutex);
    g_card_writer.dirty = true;
    if (!g_card_writer.started) {
      g_card_writer.started = true;
      // tw2de (2026-10-23): a thread of its own for good (pthread, never joined). std::thread::detach() threw on
      // the console (libnx's pthread_detach fails: std::system_error, std::terminate): the first run-time compile's
      // card write ended the process (Atmosphère: User Break in KickCardWriter).
      StartLongLivedThread(&CardWriterLoop, 256u << 10);
    }
  }
  g_card_writer.wake.notify_one();
}

}  // namespace

bool CacheHas(uint64_t key) {
  LoadCache();
  std::lock_guard<renut::nxs::NxMutex> lock(g_cache.mutex);
  return g_cache.index.Contains(key);
}

bool CacheIsFast(uint64_t key) {
  LoadCache();
  std::lock_guard<renut::nxs::NxMutex> lock(g_cache.mutex);
  return g_cache.fast.count(key) != 0;
}

void CacheAdd(uint64_t key, const std::vector<uint8_t> &dksh) {
  LoadCache();
  CacheStore(key, dksh, false);
}

#if defined(PLUME_DEKO3D)

// tw2de (2026-10-24): game shaders made from their metadata, from their GLSL, and texts read later for a variant.
std::atomic<uint64_t> g_shaders_from_meta{0}, g_shaders_from_glsl{0}, g_shader_late_reads{0};

std::unique_ptr<plume::RenderShader> CreateGameShader(plume::RenderDevice *device, uint64_t hash) {
  // tw2de (2026-10-21): the GLSL from the shader store (nx_shaders/shader_store.h), translated on the console.
  auto &store = renut::nxs::ShaderStore::Get();
  const renut::nxs::StoredShader *s = LoadShaderCacheFile() ? store.Find(hash) : nullptr;
  const std::string name = s ? fmt::format("{}_{:016X}", s->pixel ? "ps" : "vs", hash) : std::string();
  // tw2de (2026-10-24, tw2de_deko3d_shader_meta): from the metadata, without reading the text (the DKSH cache
  // and the variant lists have the keys); the text is read on the thread that needs it, for a variant to compile.
  plume::Deko3DShaderMeta meta;
  if (s && device && REXCVAR_GET(tw2de_deko3d_shader_meta) && g_shader_meta.Find(hash, s->dkKey, meta)) {
    const bool pixel = s->pixel;
    auto shader = plume::Deko3DCreateShaderFromMeta(
        device, pixel ? DkStage_Fragment : DkStage_Vertex, meta,
        [s](std::string &text) {
          g_shader_late_reads.fetch_add(1, std::memory_order_relaxed);
          return renut::nxs::ShaderStore::Get().ReadGlsl(*s, text);
        },
        [hash, pixel, default_key = s->dkKey](const std::vector<std::pair<uint32_t, uint32_t>> &values, uint64_t &key) {
          return renut::nxs::VariantRecorder::Get().KeyOf(hash, pixel, values, default_key, key);
        });
    if (shader) {
      shader->setName(name);
      g_shaders_from_meta.fetch_add(1, std::memory_order_relaxed);
      return shader;
    }
  }
  std::string glsl;
  if (!s || !device || !store.ReadGlsl(*s, glsl)) {
    static int logged = 0;
    if (logged++ < 16)
      REXLOG_ERROR("[deko3d] shader {:016X}: {}", hash, s ? "its GLSL could not be read from the store" : "not in the shader store");
    return nullptr;
  }
  std::vector<uint8_t> blob(sizeof(plume::Deko3DShaderBlob) + glsl.size());
  plume::Deko3DShaderBlob header;
  header.stage = s->pixel ? 4u : 0u;  // DkStage_Fragment / DkStage_Vertex
  header.glslSize = uint32_t(glsl.size());
  header.dkshSize = 0;
  std::memcpy(blob.data(), &header, sizeof(header));
  std::memcpy(blob.data() + sizeof(header), glsl.data(), glsl.size());
  auto shader = device->createShader(blob.data(), blob.size(), "main", plume::RenderShaderFormat::DEKO3D);
  if (shader) {
    shader->setName(name);
    g_shaders_from_glsl.fetch_add(1, std::memory_order_relaxed);
    // tw2de (2026-10-24): its metadata for the next session (written by the card writer thread).
    if (REXCVAR_GET(tw2de_deko3d_shader_meta) && plume::Deko3DShaderMetaFromGlsl(glsl, meta)) {
      g_shader_meta.Add(hash, meta);
      KickCardWriter();
    }
  }
  return shader;
}

// tw2de (2026-11-05): deko3d.log's writer: lines queued by any thread, written and flushed by its own thread (the
// card write off the caller); WriteNow writes the queue and the line on the caller, flushed (failures).
class DiagLogWriter {
 public:
  static DiagLogWriter &Instance() {
    static DiagLogWriter w;
    return w;
  }
  void Open(const std::string &path) {
    std::lock_guard lock(file_mutex_);
    if (!file_)
      file_ = std::fopen(path.c_str(), "w");
  }
  void Queue(const char *line) {
    bool start = false;
    {
      std::lock_guard lock(queue_mutex_);
      queue_.emplace_back(line);
      start = !started_;
      started_ = true;
    }
    if (start)
      tw2de::renut_gpu::StartLongLivedThread([] { Instance().Loop(); }, 32 * 1024);
    cv_.notify_one();
  }
  void WriteNow(const char *line) {
    std::vector<std::string> lines;
    {
      std::lock_guard lock(queue_mutex_);
      lines.swap(queue_);
    }
    std::lock_guard lock(file_mutex_);
    if (!file_)
      return;
    for (const std::string &l : lines)
      std::fprintf(file_, "%s\n", l.c_str());
    std::fprintf(file_, "%s\n", line);
    std::fflush(file_);
  }

 private:
  void Loop() {
    for (;;) {
      std::vector<std::string> lines;
      {
        std::unique_lock lock(queue_mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        lines.swap(queue_);
      }
      std::lock_guard lock(file_mutex_);
      if (!file_)
        continue;
      for (const std::string &l : lines)
        std::fprintf(file_, "%s\n", l.c_str());
      std::fflush(file_);
    }
  }
  std::mutex queue_mutex_, file_mutex_;
  std::condition_variable cv_;
  std::vector<std::string> queue_;
  bool started_ = false;
  FILE *file_ = nullptr;
};

void InstallShaderCache() {
  // The back end's messages into this log (plume writes them to stderr otherwise), and into deko3d.log next to the NRO,
  // flushed at each line: the log's buffer is lost when the process dies.
  DiagLogWriter::Instance().Open(NextToExecutable("log/deko3d.log").string());  // tw2de: log/
  plume::SetDeko3DLogCallback([](const char *message) {
    // tw2de (2026-10-25): the back end's progress reports (its memory, the compile threads, the swapchain, the
    // first presents, traces) in the game log only with log_diagnostics; anything else it says (a failure, a guard, a
    // fence that does not signal, a shader not loaded) always. deko3d.log keeps every line.
    static constexpr const char *kProgress[] = {
        "device: creating", "image heap:", "buffer arena:", "device ready:", "memory block:", "uam thread:",
        "uam: compile", "uam: run-time variant", "uam background thread:", "uam queue:", "shader code: block",
        "trace ", "self-test", "swapchain:", "present ",
    };
    bool progress = false;
    for (const char *prefix : kProgress)
      progress = progress || std::strncmp(message, prefix, std::strlen(prefix)) == 0;
    if (!progress || REXCVAR_GET(log_diagnostics))
      REXLOG_INFO("[deko3d] {}", message);
    // tw2de (2026-11-05): the progress lines go to deko3d.log from a writer thread (DiagLogWriter), not from the
    // thread that said them: each line was a write and a flush of the card there, the render thread's for the "memory
    // block" lines of an area entry's new buffers (run 9c361b46: 16 ms of such writes in one frame). A failure's line
    // is written and flushed here at once, after the queued ones, so a crash right after it still has it in the file.
    const auto write_start = std::chrono::steady_clock::now();
    if (progress)
      DiagLogWriter::Instance().Queue(message);
    else
      DiagLogWriter::Instance().WriteNow(message);
    ::renut::gpu::rtw::g_log_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - write_start)
                     .count()),
        std::memory_order_relaxed);
  });
  // tw2de (2026-10-18, GPU bandwidth): depth compression and Zcull, which the device and its textures take.
  plume::Deko3DOptions options;
  options.depthCompression = Deko3DDepthCompressionOn();
  options.msCompression = REXCVAR_GET(tw2de_deko3d_ms_compression);    // tw2de (2026-10-22)
  options.imageReserveChunks = uint32_t(std::max(0, int(REXCVAR_GET(tw2de_deko3d_image_reserve_chunks))));
  options.bufferArenaMB = uint32_t(std::clamp(int(REXCVAR_GET(tw2de_deko3d_buffer_arena_mb)), 0, 2048));
  options.alphaToCoverageDither = REXCVAR_GET(tw2de_deko3d_a2c_dither);  // tw2de (2026-10-22)
  options.zcull = Deko3DZcullOn();
  options.vsOutputs = Deko3DVsOutputsOn();
  options.uamFlags = Deko3DUamFlags();
  options.mergeBarriers = Deko3DBarrierMergeOn();  // tw2de (2026-10-19, GPU)
  options.leanBarriers = Deko3DBarrierLeanOn();
  options.barrierDiag = REXCVAR_GET(tw2de_diag_barriers);  // tw2de (2026-11-12)
  options.tiledCache = REXCVAR_GET(tw2de_deko3d_tiled_cache);  // TW2 (2026-11-12): a trial
  // The uam thread on the background compile core, below the game's threads (2026-10-18).
  options.compileCore = Deko3DCompileCore();
  options.compilePriority = Deko3DCompilePriority();
  options.fastRuntimeCompiles = REXCVAR_GET(tw2de_deko3d_fast_runtime_compiles);  // tw2de (2026-10-21)
  options.backgroundFullPass = false;  // the next start's pre-pass compiles the fast passes in full (variant list)
  // tw2de (2026-10-21): the uam threads off newlib's one malloc lock (thread_alloc.h), as the pre-pass's are:
  // there they waited up to two thirds of each compile for it, and here the game's threads take it too.
  options.threadAllocBegin = &renut::nxs::ThreadAllocBegin;
  options.threadAllocEnd = &renut::nxs::ThreadAllocEnd;
  options.threadAllocActive = &renut::nxs::ThreadAllocSetActive;
  plume::Deko3DSetOptions(options);
  LoadCache();
  // tw2de (2026-11-06): Renut's own shaders' DKSH, made at build time with devkitPro's uam and packed into the NRO's
  // romfs as shaders/<name>.dksh (src/renut_engine/gpu/gpu.cmake renut_host_shader; the blob's name, StageNamed): made
  // in the same build as the blob, from the same text. The 17 without specialization constants are in the NRO as
  // their metadata only (no text: plume's StageMetaOnly blob); the 2 with constants keep their text for their other
  // variants. Built without devkitPro's uam: every one as its text, compiled on the console as before.
  plume::SetDeko3DBuiltinDkshLookup([](const std::string &shader_name, std::vector<uint8_t> &dksh) {
    const std::string name = "shaders/" + shader_name + ".dksh";
    const std::vector<uint8_t> *file = tw2de::romfs::Find(name.c_str());  // tw2de (2026-11-09): read at start
    static std::atomic<uint32_t> found{0}, missing{0};
    if (!file) {
      if (missing.fetch_add(1) < 4)
        REXLOG_WARN("[deko3d] a host shader's DKSH is not in the NRO ({})", name);
      return false;
    }
    dksh = *file;
    const bool ok = !dksh.empty();
    if (!ok)
      dksh.clear();
    else if (found.fetch_add(1) == 0)
      REXLOG_INFO("[deko3d] host shaders' DKSH from the NRO (made at build time with uam)");
    return ok;
  });
  plume::SetDeko3DShaderCache(
      [](uint64_t key, std::vector<uint8_t> &dksh, bool &fast) { return CacheLookup(key, dksh, &fast); },
      [](uint64_t key, const std::vector<uint8_t> &dksh, bool fast) {
        CacheStore(key, dksh, true, fast, /*defer=*/true);
        KickCardWriter();
      });
  // tw2de (2026-10-21): the variants played, for the next start's pre-pass (nx_shaders/variant_list.h): each game
  // shader's variant with constants of its own, and every fast pass. A release can ship a whole play-through's list.
  renut::nxs::VariantRecorder::Get().Open(ShaderDataPath("tw2de_variants.txt").string());
  // tw2de (2026-10-24, tw2de_deko3d_shader_meta): the variants' keys (the pre-pass has made the lists' keys this
  // translation's) and the shaders' metadata, so a shader is made without reading its GLSL.
  if (REXCVAR_GET(tw2de_deko3d_shader_meta)) {
    // TW2 (2026-11-16): the motion-vector twins' keys are made from these (variant_list.h MotionTwinKey).
    renut::nxs::VariantRecorder::Get().LoadKeys({ShaderDataPath("tw2de_variants.txt").string(),
                                                 ShaderDataPath("tw2de_variants_release.txt").string()},
                                                renut::nxs::TranslatorTag());
    g_shader_meta.Open(ShaderDataPath("tw2de_shader_meta.bin").string());
    if (LoadShaderCacheFile())
      g_shader_meta.Prefill();  // tw2de (2026-10-24)
  }
  renut::nxs::VariantRecorder::Get().SetDeferred(&KickCardWriter);  // tw2de (2026-10-23)
  plume::SetDeko3DVariantRecorder([](const std::string &name, uint32_t stage,
                                     const std::vector<std::pair<uint32_t, uint32_t>> &values, uint64_t key, bool fast) {
    if (REXCVAR_GET(tw2de_diag_dksh_keys)) {
      static std::mutex m;
      static std::unordered_set<uint64_t> seen;
      std::lock_guard<std::mutex> lock(m);
      if (seen.insert(key).second)
        REXLOG_INFO("[dkshkey] {} {:016X} {}", name, key, values.size());
    }
    if (values.empty() && !fast)
      return;  // a default variant compiled in full: the pre-pass's own
    if (name.size() != 19 || (name.compare(0, 3, "ps_") != 0 && name.compare(0, 3, "vs_") != 0))
      return;  // not a game shader (CreateGameShader's names: ps_ / vs_ and the hash)
    renut::nxs::VariantEntry e;
    e.tag = renut::nxs::TranslatorTag();
    e.key = key;
    e.hash = std::strtoull(name.c_str() + 3, nullptr, 16);
    e.pixel = stage == 4u;  // DkStage_Fragment
    e.values = values;
    renut::nxs::VariantRecorder::Get().Record(e);
  });
  // tw2de (2026-10-23, late specialisation: nx_shaders/late_spec.h): a run-time variant's fast pass from the
  // shader's front end the pre-pass kept, when there is one.
  if (REXCVAR_GET(tw2de_deko3d_late_spec)) {
    renut::nxs::FrontEndStore::Get().Open(ShaderDataPath("tw2de_shaders_fe").string());
    plume::SetDeko3DLateCompile([](DkStage stage, uint64_t text_key,
                                   const std::vector<std::pair<uint32_t, uint32_t>> &values, std::vector<uint8_t> &dksh,
                                   std::string &log) -> bool {
      static uint32_t done = 0, no_front_end = 0, not_late = 0, failed = 0;
      auto report = [](const char *what) {
        const uint32_t n = done + no_front_end + not_late + failed;
        if (n <= 8 || n % 50 == 0)
          REXLOG_INFO("[deko3d] late specialisation: {} ({} from front ends, {} without one, {} with a constant it "
                      "cannot give, {} failed)",
                      what, done, no_front_end, not_late, failed);
      };
      if (stage != DkStage_Vertex && stage != DkStage_Fragment)
        return false;
      const uint64_t t0 = armGetSystemTick();
      std::vector<uint8_t> blob;
      if (!renut::nxs::FrontEndStore::Get().Read(text_key, blob)) {
        no_front_end++;
        report("no front end for the shader: compiled from its GLSL");
        return false;
      }
      renut::nxs::LateSpecInputs in;
      if (!renut::nxs::LateSpecPrepare(blob, values, in)) {
        not_late++;
        report("a constant the late path cannot give: compiled from its GLSL");
        return false;
      }
      const uint64_t t1 = armGetSystemTick();
      uam::Result r = uam::compileSpecialized(stage == DkStage_Fragment ? uam::Stage::Fragment : uam::Stage::Vertex,
                                              in.front_end, in.front_end_size, renut::nxs::kLateSpecBinding + 1,
                                              in.slots.data(), unsigned(in.slots.size()), in.early_z, in.gpr_cap);
      if (!r.ok || r.dksh.empty()) {
        failed++;
        REXLOG_WARN("[deko3d] late specialisation failed (compiled from its GLSL instead): {}", r.log.substr(0, 300));
        return false;
      }
      done++;
      const double tick_ms = 1000.0 / double(armGetSystemTickFreq());
      static char line[160];
      snprintf(line, sizeof(line), "%s variant in %.1f ms (%.1f ms reading its front end)",
               stage == DkStage_Fragment ? "pixel" : "vertex", double(armGetSystemTick() - t0) * tick_ms,
               double(t1 - t0) * tick_ms);
      report(line);
      dksh = std::move(r.dksh);
      log = std::move(r.log);
      return true;
    });
  }
}

std::string Report() {
  if (REXCVAR_GET(tw2de_diag_barriers))
    REXLOG_INFO("{}", plume::Deko3DBarrierDiagTake());
  const plume::Deko3DStats &st = plume::GetDeko3DStats();
  const uint64_t compiles = st.shaderCompiles;
  return fmt::format("deko3d: {} draws ({} through the address window), {} pipeline binds ({} state binds); uam {} compiles (avg {:.0f} ms, {} failed), "
                     "DKSH cache {} hits, {} added; memory: images {} MB, buffers {} MB, code {} KB, commands {} KB; "
                     "barriers: {} primitives, {} fragments, {} full ({} invalidating textures); copy and 2D engines: {} "
                     "buffer copies, {} texture uploads, {} readbacks, {} image copies, {} resolves, {} blits; game "
                     "shaders made from their metadata {}, from their GLSL {}, GLSL read later for a variant {}",
                     st.draws.load(), st.windowDraws.load(), st.pipelineBinds.load(), st.stateBinds.load(), compiles,
                     compiles ? double(st.shaderCompileNs.load()) / 1e6 / double(compiles) : 0.0,
                     st.shaderCompileFailures.load(), g_cache.hits, g_cache.added, st.imageBytes.load() >> 20,
                     st.bufferBytes.load() >> 20, st.codeBytes.load() >> 10, st.commandBytes.load() >> 10,
                     st.barriersPrimitives.load(), st.barriersFragments.load(), st.barriersFull.load(),
                     st.barriersImageInvalidate.load(), st.copyBuffer.load(), st.copyUpload.load(), st.copyReadback.load(),
                     st.copyImage.load(), st.copyResolve.load(), st.copyBlit.load(), g_shaders_from_meta.load(),
                     g_shaders_from_glsl.load(), g_shader_late_reads.load());
}

#else

std::unique_ptr<plume::RenderShader> CreateGameShader(plume::RenderDevice *, uint64_t) { return nullptr; }
void InstallShaderCache() {}
std::string Report() { return {}; }

#endif

bool BoolSpecMask(uint64_t hash, uint32_t mask[8]) {
  const renut::nxs::StoredShader *s = LoadShaderCacheFile() ? renut::nxs::ShaderStore::Get().Find(hash) : nullptr;
  if (!s)
    return false;
  std::memcpy(mask, s->boolMask, sizeof(s->boolMask));
  return true;
}

void SaveShaderCache() {  // tw2de: every batch out (the pre-pass's end, the game's exit)
  // tw2de (2026-10-20): to the card, not only to the file system service; and <path>.end. After a batch being
  // written (SyncCacheOut keeps them in order).
  std::unique_lock<renut::nxs::NxMutex> lock(g_cache.mutex);
  if (g_cache.file.IsOpen())
    SyncCacheOut(g_cache, lock);
  // tw2de (2026-10-22): the pre-pass's full compiles replace fast passes in a batch, written here: the list of
  // fast passes after it, or the next start compiled the same variants again (and again).
  if (g_cache.file.IsOpen() && g_cache.fast_dirty && g_cache.synced_end == g_cache.end)
    WriteFastLocked(g_cache);
  // tw2de (2026-10-23): the records' places, so the next start does not walk them.
  if (g_cache.file.IsOpen() && g_cache.synced_end == g_cache.end)
    WriteIndexLocked(g_cache);
  // Everything is on the card: the pre-pass's batch buffers go (the game's compiles are written one by one).
  std::vector<uint8_t>().swap(g_cache.spare);
  if (g_cache.pending.empty())
    std::vector<uint8_t>().swap(g_cache.pending);
  // tw2de (2026-10-24): and the variant list and the shader metadata (the card writer writes them after compiles;
  // here at the game's exit too), outside the cache's lock.
  lock.unlock();
  renut::nxs::VariantRecorder::Get().Flush();
#if defined(PLUME_DEKO3D)
  g_shader_meta.Flush();
#endif
}

}  // namespace tw2de::renut_gpu::deko3d

// tw2de (2026-10-23): the game's shader containers as the pre-pass found them, kept next to the NRO in a file of
// their own (tw2de_shader_containers.idx / .dat), independent of the translation. A new translation (TranslatorTag: a
// translator change or a new shader_common_glsl.h) starts the shader store over, and the store alone would have the
// pre-pass read every game file again to find the containers (gigabytes from the card): with this file it translates
// the containers it has (93 MB, one read) and scans only the game files it has not seen.
//
// .dat: the containers' bytes, one after another (virtual part, then physical part, as the game has them).
// .idx: a 16-byte header ("FXSCIDX1", version), then records u32 type, u32 payload size, payload:
//   1  a game file whose containers are all in: u64 file size, then its name (relative to the game folder)
//   2  a container: u64 hash (XXH3, the store's key), u64 offset in the .dat, u32 size
// Appended in batches, the .dat first: a record cut short at the end (power off while writing), or one whose bytes are
// not all in the .dat, and everything after it is ignored and written over. Read at start: only the .idx (a few
// hundred KB); the .dat when a new translation needs the containers. Containers the game creates at run time
// (default.xex's) are added too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "renut_engine/gpu/nx_shaders/nx_file.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"
#include "renut_engine/gpu/nx_shaders/sorted_index.h"

namespace renut::nxs {

class ContainerCache {
 public:
  static ContainerCache &Get();

  // Loads the index (the containers' places, not their bytes) or starts the files: base + ".idx" / ".dat". Once;
  // later calls return the first one's result.
  bool Open(const std::string &base);
  bool IsOpen();

  bool FileScanned(const std::string &name, uint64_t size);
  void MarkFileScanned(const std::string &name, uint64_t size);
  bool Has(uint64_t hash);
  // Adds a container (hash: XXH3 of it, the store's key); one already there is not added again. Thread-safe.
  void Add(uint64_t hash, const uint8_t *container, size_t size);
  // Writes what is buffered to the card.
  void Sync();

  size_t Count();
  // Every container's hash.
  std::vector<uint64_t> Hashes();
  // Reads every container and calls fn(hash, bytes, size) for each one whose hash 'want' accepts, from 'threads'
  // threads at once (run(n, body) starts them). Returns the number given to fn.
  size_t ForEachContainer(const std::function<bool(uint64_t)> &want,
                          const std::function<void(uint64_t, const uint8_t *, size_t)> &fn,
                          const std::function<void(const std::function<void(int)> &)> &run);

 private:
  struct Entry {
    uint64_t hash;
    int64_t offset;  // of the payload
    uint32_t size;
  };
  bool LoadLocked();
  bool StartLocked();
  void AppendIndexLocked(uint32_t type, const void *payload, size_t size, const void *extra = nullptr,
                         size_t extra_size = 0);
  void SyncLocked();

  NxMutex mutex_;
  bool tried_ = false, ok_ = false;
  std::string idx_path_, dat_path_;
  NxFile idx_, dat_;
  int64_t idx_end_ = 0, dat_end_ = 0;  // where the next bytes go (after the buffered ones are written)
  std::vector<uint8_t> pending_idx_, pending_dat_;
  std::vector<Entry> entries_;
  SortedIndex<uint8_t> hashes_;  // tw2de (2026-10-23): sorted, binary search (sorted_index.h)
  std::unordered_set<std::string> scanned_;  // "size:name"
};

}  // namespace renut::nxs

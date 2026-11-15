// tw2de (2026-10-21): the Switch's shader store: every game shader translated on the console (xenos_glsl.h), next
// to the NRO. The renderer's shader metadata (FindShaderEntry) and the deko3d back end's GLSL (CreateGameShader) come
// from it; the pre-pass (shader_prepass.h) fills it from the game files and the renderer adds the shaders the game
// creates at run time.
//
// Two files (version 2), through libnx's file calls (nx_file.h):
//   tw2de_shaders.idx  a header ("FXSIDX2", version, the translator's tag), then records (u32 type, u32 payload size,
//                       payload): each shader's metadata with where its GLSL is in the .dat, the game files scanned,
//                       the containers that do not translate. Read whole at start: a few MB.
//   tw2de_shaders.dat  a header ("FXSDAT2", the tag), then each shader's GLSL, zstd with shader_common_glsl.h as the
//                       dictionary (read one shader at a time, when the renderer creates it).
// Both are sized ahead (fsFsCreateFile / fsFileSetSize, nothing written into the space) and written in batches. The
// .idx's records end where tw2de_shaders.idx.end says, which is written only once the records before it, and the GLSL
// they point to, are on the card: past it, nothing is a record. A store made by another translation (TranslatorTag)
// is started over; one of version 1 (tw2de_shaders.bin, one file) is converted once and deleted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "renut_engine/gpu/nx_shaders/xenos_glsl.h"
#include "renut_engine/gpu/nx_shaders/nx_file.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"
#include "renut_engine/gpu/nx_shaders/sorted_index.h"

struct ZSTD_CDict_s;
struct ZSTD_DDict_s;

namespace renut::nxs {

struct StoredShader {
  ShaderCacheEntry entry{};  // vertexInputOffset / literalOffset into the store's arrays
  bool pixel = false;
  uint32_t boolMask[8] = {};
  uint64_t dkKey = 0;        // fnv1a64 of the GLSL: the default variant's key in the DKSH cache (plume_deko3d)
  uint32_t glslSize = 0;
  uint32_t zSize = 0;
  uint64_t zOffset = 0;      // of the compressed GLSL in the .dat
};

class ShaderStore {
 public:
  static ShaderStore &Get();

  // Loads the file (or starts it). Once; later calls return the first one's result.
  bool Open(const std::string &path);
  bool IsOpen() { return idx_.IsOpen(); }

  const StoredShader *Find(uint64_t hash) const;
  // Adds a translation (to the file and the index); thread-safe. The stored shader, or null when it could not be
  // written. One already there is returned as it is.
  const StoredShader *Add(const TranslatedShader &shader);
  void AddFailure(uint64_t hash);
  bool Failed(uint64_t hash) const;

  // The pre-pass's progress: a game file all of whose containers are in.
  bool FileScanned(const std::string &name, uint64_t size) const;
  void MarkFileScanned(const std::string &name, uint64_t size);

  bool ReadGlsl(const StoredShader &shader, std::string &glsl);

  // tw2de (2026-10-21): the whole .dat in memory (one card read) for the pre-pass's compiles, so ReadGlsl never
  // waits for the card (whose lock the DKSH cache's batch writes hold); and back. Not while shaders are added.
  // The bytes held, or 0 when it could not be read (ReadGlsl reads the card then, as before).
  size_t LoadGlslIntoMemory();
  void ReleaseGlslMemory();

  // Every shader (the pre-pass's compile list). Not while others are added.
  void ForEach(const std::function<void(const StoredShader &)> &fn) const;
  size_t Count() const;

  // Writes buffered records through to the card.
  void Sync();

  const ShaderVertexInput *VertexInputs() const { return inputs_.data(); }
  const ShaderLiteral *Literals() const { return literals_.data(); }
  const std::string &Path() const { return path_; }

 private:
  bool Load();
  bool Start();
  bool MigrateV1();
  bool ParseRecords(const uint8_t *data, size_t size, bool v1, int64_t v1_base);
  // A record into the .idx batch (and, for a shader, its compressed GLSL into the .dat batch): under file_mutex_.
  void AppendLocked(uint32_t type, const std::vector<uint8_t> &payload);
  int64_t AppendDatLocked(const uint8_t *data, size_t size);
  // tw2de (2026-10-21): the batches out to the card. Called with file_mutex_ held (lock); the batch is taken
  // under it, then written without it (other threads go on adding to the next batch), under CardMutex() and
  // write_mutex_ (batches reach the card in order). force: whatever is pending; else only when the batch is
  // kBatchBytes or kBatchSeconds old.
  void SyncOut(std::unique_lock<NxMutex> &lock, bool force);
  void SyncLocked();  // at load: everything, now (one thread)
  const StoredShader *Index(StoredShader &&s, const ShaderVertexInput *inputs, const ShaderLiteral *literals);

  std::string path_;      // the version 1 file's name (tw2de_shaders.bin); the others are made from it
  std::string idx_path_, dat_path_;
  NxFile idx_, dat_;
  int64_t idx_end_ = 0, dat_end_ = 0;   // where the next record / GLSL goes
  int64_t idx_synced_ = -1;             // what .idx.end says
  std::vector<uint8_t> idx_pending_, dat_pending_;  // the batch, from idx_pending_at_ / dat_pending_at_
  int64_t idx_pending_at_ = 0, dat_pending_at_ = 0;
  std::unique_ptr<uint8_t[]> dat_memory_;           // the .dat's [0, dat_memory_size_) (LoadGlslIntoMemory)
  size_t dat_memory_size_ = 0;
  std::vector<uint8_t> dat_inflight_;               // a batch being written (ReadGlsl reads from it meanwhile)
  std::vector<uint8_t> idx_inflight_;
  // The buffers of the batch written last, cleared, kept for the next one: a batch is never regrown from nothing.
  std::vector<uint8_t> dat_spare_, idx_spare_;
  int64_t dat_inflight_at_ = 0;
  uint64_t batch_started_ = 0;                      // armGetSystemTick / steady ns of the batch's first record
  NxMutex write_mutex_;
  NxFile idx_end_file_;                             // tw2de_shaders.idx.end, open while the store is
  uint32_t unsynced_ = 0;
  bool read_only_ = false;
  mutable NxSharedMutex mutex_;  // the index
  NxMutex file_mutex_;            // the file's position
  std::deque<StoredShader> shaders_;
  // tw2de (2026-10-23): sorted by hash, found by binary search (sorted_index.h).
  SortedIndex<StoredShader *> index_;
  SortedIndex<uint8_t> failed_;
  std::unordered_map<std::string, uint64_t> scanned_;
  std::unordered_map<uint64_t, std::string> memory_glsl_;  // shaders the card had no room for
  // Fixed capacity: the renderer keeps pointers into them (g_shaderVertexInputs, g_shaderLiterals).
  std::vector<ShaderVertexInput> inputs_;
  std::vector<ShaderLiteral> literals_;
  ZSTD_CDict_s *cdict_ = nullptr;
  ZSTD_DDict_s *ddict_ = nullptr;
};

}  // namespace renut::nxs

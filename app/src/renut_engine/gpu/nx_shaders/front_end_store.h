// tw2de (2026-10-23): each shader's front end for late specialisation (late_spec.h), kept next to the shader store:
// tw2de_shaders_fe.idx / .dat, made by the pre-pass. Keyed by the shader's GLSL (StoredShader::dkKey: the FNV-1a 64 of
// its default variant's text, the key plume's default variant has).
//
// .dat: the entries' bytes, zstd, one after another.
// .idx: a 32-byte header ("FXFEIDX1", version, the translation's tag, the front end's format), then records u32 type,
// u32 payload size, payload:
//   1  an entry: u64 key, u64 offset in the .dat, u32 compressed size, u32 size
//   2  a shader without one (its GLSL has no late form, or the front end failed): u64 key
// Appended in batches, the .dat first; a record cut short, or one whose bytes are not all in the .dat, ends the file
// (written over). Another translation or format: started over. The whole of the 18,912 shaders' front ends: 175 MB,
// ~60 MB compressed one by one.
// tw2de (2026-10-24): written the way the DKSH cache is (tw2de_renut_deko3d.cpp): a batch at 8 MB or 30 s, taken
// out of the store and written without its lock (the other threads go on adding and reading; a batch being written is
// read from memory), in order, one card operation at a time; the .dat sized 64 MB ahead (its space the card's at once,
// no growth by every batch). The .idx records still come after the .dat bytes they name are flushed, so a record is
// only ever on the card with its bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "renut_engine/gpu/nx_shaders/nx_file.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"
#include "renut_engine/gpu/nx_shaders/sorted_index.h"

namespace renut::nxs {

class FrontEndStore {
 public:
  static FrontEndStore &Get();

  // base + ".idx" / ".dat". Once; later calls return the first one's result.
  bool Open(const std::string &base);
  bool IsOpen();

  // An entry or a recorded failure: nothing for the pre-pass to do.
  bool Has(uint64_t key);
  bool HasEntry(uint64_t key);
  size_t Count();
  // Thread-safe (the pre-pass's compile threads).
  void Add(uint64_t key, const std::vector<uint8_t> &bytes);
  void AddFailure(uint64_t key);
  void Sync();
  // The entry's bytes (one card read and a decompression), false when there is none.
  bool Read(uint64_t key, std::vector<uint8_t> &out);

 private:
  struct Entry {
    int64_t offset;
    uint32_t zsize, size;  // zsize 0: a failure
  };
  bool LoadLocked();
  bool StartLocked();
  // The pending batch out (lock holds mutex_; released while the card is written).
  void SyncOut(std::unique_lock<NxMutex> &lock);
  // A batch due (8 MB or 30 s since its first record): SyncOut.
  void MaybeSyncOut(std::unique_lock<NxMutex> &lock);
  void StartBatchLocked();

  NxMutex mutex_;
  NxMutex write_mutex_;  // one batch written at a time, in order
  bool tried_ = false, ok_ = false;
  std::string idx_path_, dat_path_;
  NxFile idx_, dat_;
  // The ends with every record added (written or not); where the pending batch starts.
  int64_t idx_end_ = 0, dat_end_ = 0;
  int64_t pending_idx_at_ = 0, pending_dat_at_ = 0;
  std::vector<uint8_t> pending_idx_, pending_dat_;
  // The batch being written (read from here meanwhile), and the last one's buffers to fill again.
  std::vector<uint8_t> inflight_idx_, inflight_dat_, spare_idx_, spare_dat_;
  int64_t inflight_dat_at_ = 0;
  uint64_t batch_started_ = 0;
  SortedIndex<Entry> index_;
};

}  // namespace renut::nxs

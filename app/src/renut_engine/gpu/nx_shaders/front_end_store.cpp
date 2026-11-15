// tw2de (2026-10-23): front_end_store.h.
#include "renut_engine/gpu/nx_shaders/front_end_store.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#include <zstd.h>

#include "renut_engine/gpu/nx_shaders/xenos_glsl.h"

namespace renut::nxs {

namespace {
constexpr char kMagic[8] = {'F', 'X', 'F', 'E', 'I', 'D', 'X', '1'};
constexpr uint32_t kVersion = 1;
// The front end's format: uam's front-end bytes (compiler_iface.cpp FrontEndHead) and late_spec's pack. A change to
// either, or to Mesa's front end, starts the store over.
constexpr uint64_t kFormat = 1;
constexpr uint32_t kTypeEntry = 1, kTypeFailure = 2;
constexpr size_t kHeader = 32;
// tw2de (2026-10-24): as the DKSH cache's batches (tw2de_renut_deko3d.cpp kBatchBytes / kBatchNs).
constexpr size_t kBatchBytes = 8u << 20;
constexpr uint64_t kBatchNs = 30ull * 1000000000ull;
constexpr int64_t kDatReserve = int64_t(64) << 20;  // the whole game's front ends: ~60 MB
constexpr int kLevel = 6;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

FrontEndStore &FrontEndStore::Get() {
  static FrontEndStore store;
  return store;
}

bool FrontEndStore::Open(const std::string &base) {
  std::lock_guard<NxMutex> lock(mutex_);
  if (tried_)
    return ok_;
  tried_ = true;
  idx_path_ = base + ".idx";
  dat_path_ = base + ".dat";
  ok_ = LoadLocked() || StartLocked();
  return ok_;
}

bool FrontEndStore::IsOpen() {
  std::lock_guard<NxMutex> lock(mutex_);
  return ok_;
}

bool FrontEndStore::LoadLocked() {
  if (!idx_.Open(idx_path_) || !dat_.Open(dat_path_))
    return false;
  const int64_t idx_size = idx_.Size(), dat_size = dat_.Size();
  if (idx_size < int64_t(kHeader) || dat_size < 0)
    return false;
  std::vector<uint8_t> idx(static_cast<size_t>(idx_size));
  if (!idx_.Read(0, idx.data(), idx.size()))
    return false;
  uint32_t version = 0;
  uint64_t tag = 0, format = 0;
  std::memcpy(&version, idx.data() + 8, 4);
  std::memcpy(&tag, idx.data() + 16, 8);
  std::memcpy(&format, idx.data() + 24, 8);
  if (std::memcmp(idx.data(), kMagic, 8) != 0 || version != kVersion || tag != TranslatorTag() || format != kFormat)
    return false;
  std::vector<SortedIndex<Entry>::Item> items;
  size_t p = kHeader;
  int64_t dat_used = 0;
  while (p + 8 <= idx.size()) {
    uint32_t type = 0, size = 0;
    std::memcpy(&type, idx.data() + p, 4);
    std::memcpy(&size, idx.data() + p + 4, 4);
    if (p + 8 + size > idx.size())
      break;
    const uint8_t *q = idx.data() + p + 8;
    if (type == kTypeEntry && size == 24) {
      uint64_t key;
      Entry e{};
      std::memcpy(&key, q, 8);
      std::memcpy(&e.offset, q + 8, 8);
      std::memcpy(&e.zsize, q + 16, 4);
      std::memcpy(&e.size, q + 20, 4);
      if (e.offset < 0 || e.zsize == 0 || e.offset + int64_t(e.zsize) > dat_size)
        break;  // its bytes did not all reach the card
      items.push_back({key, e});
      dat_used = std::max(dat_used, e.offset + int64_t(e.zsize));
    }
    else if (type == kTypeFailure && size == 8) {
      uint64_t key;
      std::memcpy(&key, q, 8);
      items.push_back({key, Entry{0, 0, 0}});
    }
    else {
      break;
    }
    p += 8 + size;
  }
  index_.Assign(std::move(items), true);
  idx_end_ = int64_t(p);
  dat_end_ = dat_used;
  pending_idx_at_ = idx_end_;
  pending_dat_at_ = dat_end_;
  if (dat_size < kDatReserve)
    dat_.SetSize(kDatReserve);  // a store from before the reserve: sized now (a failure only costs the growth)
  return true;
}

bool FrontEndStore::StartLocked() {
  idx_.Close();
  dat_.Close();
  NxFile::Delete(idx_path_);
  NxFile::Delete(dat_path_);
  index_.Clear();
  if (!idx_.Open(idx_path_, 0) || !dat_.Open(dat_path_, kDatReserve))
    return false;
  uint8_t head[kHeader] = {};
  std::memcpy(head, kMagic, 8);
  std::memcpy(head + 8, &kVersion, 4);
  const uint64_t tag = TranslatorTag();
  std::memcpy(head + 16, &tag, 8);
  std::memcpy(head + 24, &kFormat, 8);
  if (!idx_.Write(0, head, sizeof(head)) || !idx_.Flush())
    return false;
  idx_end_ = int64_t(kHeader);
  dat_end_ = 0;
  pending_idx_at_ = idx_end_;
  pending_dat_at_ = 0;
  return true;
}

void FrontEndStore::SyncOut(std::unique_lock<NxMutex> &lock) {
  lock.unlock();
  std::lock_guard<NxMutex> order(write_mutex_);
  lock.lock();
  if (!ok_ || (pending_idx_.empty() && pending_dat_.empty()))
    return;
  // The batch moves out (no copy; Read reads it as inflight), the spare buffers in.
  inflight_dat_.swap(pending_dat_);
  pending_dat_.swap(spare_dat_);
  inflight_idx_.swap(pending_idx_);
  pending_idx_.swap(spare_idx_);
  const int64_t dat_at = pending_dat_at_, idx_at = pending_idx_at_;
  inflight_dat_at_ = dat_at;
  pending_dat_at_ = dat_end_;
  pending_idx_at_ = idx_end_;
  lock.unlock();
  bool ok;
  {
    // The inflight buffers without mutex_: only this thread (write_mutex_) changes them; Read only reads them.
    std::lock_guard<NxMutex> card(CardMutex());
    ok = (inflight_dat_.empty() || (dat_.Write(dat_at, inflight_dat_.data(), inflight_dat_.size()) && dat_.Flush())) &&
         (inflight_idx_.empty() || (idx_.Write(idx_at, inflight_idx_.data(), inflight_idx_.size()) && idx_.Flush()));
  }
  lock.lock();
  if (ok) {
    inflight_dat_.clear();
    spare_dat_.swap(inflight_dat_);
    inflight_idx_.clear();
    spare_idx_.swap(inflight_idx_);
  }
  else {
    ok_ = false;  // no more records; the batch stays readable (inflight) for this run
  }
}

void FrontEndStore::StartBatchLocked() {
  if (pending_idx_.empty() && pending_dat_.empty()) {
    batch_started_ = NowNs();
    if (pending_dat_.capacity() < kBatchBytes + (256u << 10))
      pending_dat_.reserve(kBatchBytes + (256u << 10));  // once: then reused (with the spare)
  }
}

void FrontEndStore::MaybeSyncOut(std::unique_lock<NxMutex> &lock) {
  if (pending_dat_.size() >= kBatchBytes || NowNs() - batch_started_ >= kBatchNs)
    SyncOut(lock);
}

bool FrontEndStore::Has(uint64_t key) {
  std::lock_guard<NxMutex> lock(mutex_);
  return index_.Contains(key);
}

bool FrontEndStore::HasEntry(uint64_t key) {
  std::lock_guard<NxMutex> lock(mutex_);
  const Entry *e = index_.Find(key);
  return e && e->zsize;
}

size_t FrontEndStore::Count() {
  std::lock_guard<NxMutex> lock(mutex_);
  return index_.Size();
}

void FrontEndStore::Add(uint64_t key, const std::vector<uint8_t> &bytes) {
  if (bytes.empty() || bytes.size() > 0xFFFFFFFFu)
    return;
  std::vector<uint8_t> z(ZSTD_compressBound(bytes.size()));
  const size_t zn = ZSTD_compress(z.data(), z.size(), bytes.data(), bytes.size(), kLevel);
  if (ZSTD_isError(zn))
    return;
  std::unique_lock<NxMutex> lock(mutex_);
  if (!ok_ || index_.Contains(key))
    return;
  StartBatchLocked();
  const Entry e{dat_end_, uint32_t(zn), uint32_t(bytes.size())};
  pending_dat_.insert(pending_dat_.end(), z.data(), z.data() + zn);
  dat_end_ += int64_t(zn);
  uint8_t rec[8 + 24];
  const uint32_t type = kTypeEntry, size = 24;
  std::memcpy(rec, &type, 4);
  std::memcpy(rec + 4, &size, 4);
  std::memcpy(rec + 8, &key, 8);
  std::memcpy(rec + 16, &e.offset, 8);
  std::memcpy(rec + 24, &e.zsize, 4);
  std::memcpy(rec + 28, &e.size, 4);
  pending_idx_.insert(pending_idx_.end(), rec, rec + sizeof(rec));
  idx_end_ += int64_t(sizeof(rec));
  index_.Set(key, e);
  MaybeSyncOut(lock);
}

void FrontEndStore::AddFailure(uint64_t key) {
  std::unique_lock<NxMutex> lock(mutex_);
  if (!ok_ || index_.Contains(key))
    return;
  StartBatchLocked();
  uint8_t rec[8 + 8];
  const uint32_t type = kTypeFailure, size = 8;
  std::memcpy(rec, &type, 4);
  std::memcpy(rec + 4, &size, 4);
  std::memcpy(rec + 8, &key, 8);
  pending_idx_.insert(pending_idx_.end(), rec, rec + sizeof(rec));
  idx_end_ += int64_t(sizeof(rec));
  index_.Set(key, Entry{0, 0, 0});
  MaybeSyncOut(lock);
}

void FrontEndStore::Sync() {
  std::unique_lock<NxMutex> lock(mutex_);
  SyncOut(lock);
}

bool FrontEndStore::Read(uint64_t key, std::vector<uint8_t> &out) {
  std::vector<uint8_t> z;
  uint32_t size = 0;
  int64_t at = -1;
  {
    std::lock_guard<NxMutex> lock(mutex_);
    const Entry *e = index_.Find(key);  // written entries stay readable after a failed write
    if (!e || !e->zsize)
      return false;
    z.resize(e->zsize);
    size = e->size;
    const int64_t off = e->offset;
    if (!pending_dat_.empty() && off >= pending_dat_at_) {
      // in the batch not written yet
      const size_t from = size_t(off - pending_dat_at_);
      if (from + z.size() > pending_dat_.size())
        return false;
      std::memcpy(z.data(), pending_dat_.data() + from, z.size());
    }
    else if (!inflight_dat_.empty() && off >= inflight_dat_at_ &&
             off + int64_t(z.size()) <= inflight_dat_at_ + int64_t(inflight_dat_.size())) {
      // in the batch being written
      std::memcpy(z.data(), inflight_dat_.data() + size_t(off - inflight_dat_at_), z.size());
    }
    else {
      at = off;  // on the card: read below, without mutex_ (the adding threads go on)
    }
  }
  if (at >= 0) {
    std::lock_guard<NxMutex> card(CardMutex());
    if (!dat_.Read(at, z.data(), z.size()))
      return false;
  }
  out.resize(size);
  const size_t n = ZSTD_decompress(out.data(), out.size(), z.data(), z.size());
  return !ZSTD_isError(n) && n == size;
}

}  // namespace renut::nxs

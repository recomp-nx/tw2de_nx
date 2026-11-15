// tw2de (2026-10-23): container_cache.h.
#include "renut_engine/gpu/nx_shaders/container_cache.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace renut::nxs {

namespace {
constexpr char kMagic[8] = {'F', 'X', 'S', 'C', 'I', 'D', 'X', '1'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kTypeFile = 1, kTypeContainer = 2;
constexpr size_t kSyncBytes = 4u << 20;  // a batch reaches the card at this size (and at Sync)

std::string ScannedKey(const std::string &name, uint64_t size) { return std::to_string(size) + ":" + name; }
}  // namespace

ContainerCache &ContainerCache::Get() {
  static ContainerCache cache;
  return cache;
}

bool ContainerCache::Open(const std::string &base) {
  std::lock_guard<NxMutex> lock(mutex_);
  if (tried_)
    return ok_;
  tried_ = true;
  idx_path_ = base + ".idx";
  dat_path_ = base + ".dat";
  ok_ = LoadLocked() || StartLocked();
  return ok_;
}

bool ContainerCache::IsOpen() {
  std::lock_guard<NxMutex> lock(mutex_);
  return ok_;
}

bool ContainerCache::LoadLocked() {
  if (!idx_.Open(idx_path_) || !dat_.Open(dat_path_))
    return false;
  const int64_t idx_size = idx_.Size(), dat_size = dat_.Size();
  if (idx_size < 16 || dat_size < 0)
    return false;
  std::vector<uint8_t> idx(static_cast<size_t>(idx_size));
  if (!idx_.Read(0, idx.data(), idx.size()))
    return false;
  uint32_t version = 0;
  std::memcpy(&version, idx.data() + 8, 4);
  if (std::memcmp(idx.data(), kMagic, 8) != 0 || version != kVersion)
    return false;
  size_t p = 16;
  int64_t dat_used = 0;
  while (p + 8 <= idx.size()) {
    uint32_t type = 0, size = 0;
    std::memcpy(&type, idx.data() + p, 4);
    std::memcpy(&size, idx.data() + p + 4, 4);
    if (p + 8 + size > idx.size())
      break;
    const uint8_t *q = idx.data() + p + 8;
    if (type == kTypeFile && size >= 8) {
      uint64_t fsize = 0;
      std::memcpy(&fsize, q, 8);
      scanned_.insert(ScannedKey(std::string(reinterpret_cast<const char *>(q + 8), size - 8), fsize));
    }
    else if (type == kTypeContainer && size == 20) {
      Entry e{};
      std::memcpy(&e.hash, q, 8);
      std::memcpy(&e.offset, q + 8, 8);
      std::memcpy(&e.size, q + 16, 4);
      if (e.offset < 0 || e.offset + int64_t(e.size) > dat_size)
        break;  // its bytes did not all reach the card
      if (hashes_.Insert(e.hash, 0))
        entries_.push_back(e);
      dat_used = std::max(dat_used, e.offset + int64_t(e.size));
    }
    else {
      break;  // not a record: the end of what was written
    }
    p += 8 + size;
  }
  idx_end_ = int64_t(p);
  dat_end_ = dat_used;  // bytes past the last indexed container are written over
  return true;
}

bool ContainerCache::StartLocked() {
  idx_.Close();
  dat_.Close();
  NxFile::Delete(idx_path_);
  NxFile::Delete(dat_path_);
  entries_.clear();
  hashes_.Clear();
  scanned_.clear();
  if (!idx_.Open(idx_path_, 0) || !dat_.Open(dat_path_, 0))
    return false;
  uint8_t head[16] = {};
  std::memcpy(head, kMagic, 8);
  std::memcpy(head + 8, &kVersion, 4);
  if (!idx_.Write(0, head, sizeof(head)) || !idx_.Flush())
    return false;
  idx_end_ = 16;
  dat_end_ = 0;
  return true;
}

void ContainerCache::AppendIndexLocked(uint32_t type, const void *payload, size_t size, const void *extra,
                                       size_t extra_size) {
  const uint32_t total = uint32_t(size + extra_size);
  const size_t at = pending_idx_.size();
  pending_idx_.resize(at + 8 + total);
  std::memcpy(pending_idx_.data() + at, &type, 4);
  std::memcpy(pending_idx_.data() + at + 4, &total, 4);
  if (size)
    std::memcpy(pending_idx_.data() + at + 8, payload, size);
  if (extra_size)
    std::memcpy(pending_idx_.data() + at + 8 + size, extra, extra_size);
}

void ContainerCache::SyncLocked() {
  if (!ok_ || (pending_idx_.empty() && pending_dat_.empty()))
    return;
  // The containers' bytes first, so no index record names bytes the card does not have.
  if (!pending_dat_.empty()) {
    if (!dat_.Write(dat_end_, pending_dat_.data(), pending_dat_.size()) || !dat_.Flush()) {
      ok_ = false;
      return;
    }
    dat_end_ += int64_t(pending_dat_.size());
    pending_dat_.clear();
  }
  if (!pending_idx_.empty()) {
    if (!idx_.Write(idx_end_, pending_idx_.data(), pending_idx_.size()) || !idx_.Flush()) {
      ok_ = false;
      return;
    }
    idx_end_ += int64_t(pending_idx_.size());
    pending_idx_.clear();
  }
}

bool ContainerCache::FileScanned(const std::string &name, uint64_t size) {
  std::lock_guard<NxMutex> lock(mutex_);
  return ok_ && scanned_.count(ScannedKey(name, size)) != 0;
}

void ContainerCache::MarkFileScanned(const std::string &name, uint64_t size) {
  std::lock_guard<NxMutex> lock(mutex_);
  if (!ok_ || !scanned_.insert(ScannedKey(name, size)).second)
    return;
  AppendIndexLocked(kTypeFile, &size, 8, name.data(), name.size());
  // The file's containers were added before its mark: both reach the card together, in that order.
  SyncLocked();
}

bool ContainerCache::Has(uint64_t hash) {
  std::lock_guard<NxMutex> lock(mutex_);
  return hashes_.Contains(hash);
}

void ContainerCache::Add(uint64_t hash, const uint8_t *container, size_t size) {
  std::lock_guard<NxMutex> lock(mutex_);
  if (!ok_ || size == 0 || size > 0xFFFFFFFFu || !hashes_.Insert(hash, 0))
    return;
  Entry e{hash, dat_end_ + int64_t(pending_dat_.size()), uint32_t(size)};
  pending_dat_.insert(pending_dat_.end(), container, container + size);
  uint8_t rec[20];
  std::memcpy(rec, &e.hash, 8);
  std::memcpy(rec + 8, &e.offset, 8);
  std::memcpy(rec + 16, &e.size, 4);
  AppendIndexLocked(kTypeContainer, rec, sizeof(rec));
  entries_.push_back(e);
  if (pending_dat_.size() >= kSyncBytes)
    SyncLocked();
}

void ContainerCache::Sync() {
  std::lock_guard<NxMutex> lock(mutex_);
  SyncLocked();
}

size_t ContainerCache::Count() {
  std::lock_guard<NxMutex> lock(mutex_);
  return entries_.size();
}

std::vector<uint64_t> ContainerCache::Hashes() {
  std::lock_guard<NxMutex> lock(mutex_);
  std::vector<uint64_t> out;
  out.reserve(entries_.size());
  for (const Entry &e : entries_)
    out.push_back(e.hash);
  return out;
}

size_t ContainerCache::ForEachContainer(const std::function<bool(uint64_t)> &want,
                                        const std::function<void(uint64_t, const uint8_t *, size_t)> &fn,
                                        const std::function<void(const std::function<void(int)> &)> &run) {
  std::vector<Entry> todo;
  std::vector<uint8_t> data;
  {
    std::lock_guard<NxMutex> lock(mutex_);
    if (!ok_)
      return 0;
    SyncLocked();
    for (const Entry &e : entries_)
      if (want(e.hash))
        todo.push_back(e);
    if (todo.empty())
      return 0;
    // The whole .dat in one read (the card's best case), then the containers from memory.
    data.resize(size_t(dat_end_));
    if (data.empty() || !dat_.Read(0, data.data(), data.size()))
      return 0;
  }
  std::atomic<size_t> next{0};
  run([&](int) {
    for (size_t i; (i = next.fetch_add(1)) < todo.size();)
      fn(todo[i].hash, data.data() + todo[i].offset, todo[i].size);
  });
  return todo.size();
}

}  // namespace renut::nxs

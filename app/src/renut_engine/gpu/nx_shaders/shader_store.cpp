// tw2de (2026-10-21): shader_store.h.
#include "renut_engine/gpu/nx_shaders/shader_store.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(tw2de_shader_store_reserve_mb, 64, "tw2de",
                     "Switch: the shader store's GLSL file (tw2de_shaders.dat) is made this long ahead (its index "
                     "an eighth of it), so the pre-pass writes into space the SD card already has (0: grows as needed)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::nxs {

namespace {

constexpr char kIdxMagic[8] = {'F', 'X', 'S', 'I', 'D', 'X', '2', 0};
constexpr char kDatMagic[8] = {'F', 'X', 'S', 'D', 'A', 'T', '2', 0};
constexpr char kV1Magic[8] = {'F', 'X', 'S', 'T', 'O', 'R', 'E', '1'};
constexpr uint32_t kVersion = 2;
constexpr int64_t kHeaderSize = 32;  // magic, version, pad, the translator's tag, reserved

enum : uint32_t {
  kRecordShader = 0x52444853u,   // "SHDR"
  kRecordFile = 0x454C4946u,     // "FILE": a game file scanned
  kRecordFailure = 0x4C494146u,  // "FAIL": a container that does not translate
};

constexpr int kZstdLevel = 9;
// Room for the shaders the game creates at run time (default.xex's) and later passes: the arrays never grow (the
// renderer reads them without a lock).
constexpr size_t kInputHeadroom = 1u << 16;
constexpr size_t kLiteralHeadroom = 1u << 18;
// tw2de (2026-10-21): the batches go to the card at 8 MB or 30 s (a flush every 64 shaders made ~300 rounds of
// three flushes and an open, each interleaved with the scan's reads); a run cut short loses at most that.
constexpr size_t kBatchBytes = 8u << 20;
constexpr uint64_t kBatchSeconds = 30;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

#pragma pack(push, 1)
// Version 1 (one file): the record head, then the inputs, the literals and the compressed GLSL.
struct ShaderRecordHeadV1 {
  uint64_t hash;
  uint32_t pixel, glslSize, zSize, inputCount, literalCount;
  uint32_t boolMask[8];
  uint64_t dkKey;
  ShaderCacheEntry entry;
};
// Version 2 (.idx): the same, and where the compressed GLSL is in the .dat; then the inputs and the literals.
struct ShaderRecordHead {
  ShaderRecordHeadV1 v1;
  uint64_t zOffset;
};
#pragma pack(pop)

template <class T>
void Put(std::vector<uint8_t> &v, const T &x) {
  const auto *p = reinterpret_cast<const uint8_t *>(&x);
  v.insert(v.end(), p, p + sizeof(T));
}

void MakeHeader(uint8_t (&head)[kHeaderSize], const char (&magic)[8]) {
  std::memset(head, 0, sizeof(head));
  std::memcpy(head, magic, 8);
  std::memcpy(head + 8, &kVersion, 4);
  const uint64_t tag = TranslatorTag();
  std::memcpy(head + 16, &tag, 8);
}

bool HeaderOk(const uint8_t (&head)[kHeaderSize], const char (&magic)[8], uint32_t version) {
  uint32_t v = 0;
  uint64_t tag = 0;
  std::memcpy(&v, head + 8, 4);
  std::memcpy(&tag, head + 16, 8);
  return std::memcmp(head, magic, 8) == 0 && v == version && tag == TranslatorTag();
}

int64_t ReadEnd(const std::string &path) {
  NxFile f;
  int64_t end = -1;
  if (!f.OpenRead(path) || !f.Read(0, &end, sizeof(end)))
    end = -1;
  return end;
}

}  // namespace

ShaderStore &ShaderStore::Get() {
  static ShaderStore store;
  return store;
}

bool ShaderStore::Open(const std::string &path) {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [&] {
    path_ = path;
    const std::string base = path.size() > 4 && path.compare(path.size() - 4, 4, ".bin") == 0
                                 ? path.substr(0, path.size() - 4)
                                 : path;
    idx_path_ = base + ".idx";
    dat_path_ = base + ".dat";
    index_.Reserve(32768);  // The game has 18,791 shaders: the array is not regrown under the index's lock
    const std::string_view header = GlslHeader();
    cdict_ = ZSTD_createCDict(header.data(), header.size(), kZstdLevel);
    ddict_ = ZSTD_createDDict(header.data(), header.size());
    ok = cdict_ && ddict_ && (Load() || (Start() && (MigrateV1(), true)));
  });
  return ok;
}

bool ShaderStore::Start() {
  idx_.Close();
  dat_.Close();
  shaders_.clear();
  index_.Clear();
  failed_.Clear();
  scanned_.clear();
  inputs_.clear();
  literals_.clear();
  inputs_.reserve(kInputHeadroom);
  literals_.reserve(kLiteralHeadroom);
  const int64_t reserve = int64_t(std::max(0, int(REXCVAR_GET(tw2de_shader_store_reserve_mb)))) << 20;
  const int64_t dat_size = std::max(kHeaderSize, reserve), idx_size = std::max(kHeaderSize, reserve / 8);
  // Made the reserve's size at once (fsFsCreateFile: nothing written), or an old file sized to it.
  if (!dat_.Open(dat_path_, dat_size) || !idx_.Open(idx_path_, idx_size))
    return false;
  if (dat_.Size() < dat_size)
    dat_.SetSize(dat_size);
  if (idx_.Size() < idx_size)
    idx_.SetSize(idx_size);
  uint8_t head[kHeaderSize];
  MakeHeader(head, kDatMagic);
  if (!dat_.Write(0, head, sizeof(head)))
    return false;
  MakeHeader(head, kIdxMagic);
  if (!idx_.Write(0, head, sizeof(head)))
    return false;
  idx_end_ = dat_end_ = kHeaderSize;
  idx_synced_ = -1;
  read_only_ = false;
  std::lock_guard<NxMutex> lock(file_mutex_);
  SyncLocked();  // .idx.end: no records
  return true;
}

// The records of an .idx (v1: of the one file, whose shaders carry their GLSL: copied into the .dat batch).
bool ShaderStore::ParseRecords(const uint8_t *data, size_t size, bool v1, int64_t v1_base) {
  std::vector<ShaderVertexInput> inputs;
  std::vector<ShaderLiteral> literals;
  struct Pending {
    StoredShader s;
    size_t input_at, literal_at;
  };
  std::vector<Pending> pending;
  size_t at = 0;
  while (at + 8 <= size) {
    uint32_t type, len;
    std::memcpy(&type, data + at, 4);
    std::memcpy(&len, data + at + 4, 4);
    if (len > (64u << 20) || at + 8 + len > size)
      break;
    const uint8_t *p = data + at + 8;
    bool good = true;
    const size_t head_size = v1 ? sizeof(ShaderRecordHeadV1) : sizeof(ShaderRecordHead);
    if (type == kRecordShader && len >= head_size) {
      ShaderRecordHead h{};
      std::memcpy(&h, p, head_size);
      const ShaderRecordHeadV1 &hv = h.v1;
      const size_t need = head_size + size_t(hv.inputCount) * sizeof(ShaderVertexInput) +
                          size_t(hv.literalCount) * sizeof(ShaderLiteral) + (v1 ? hv.zSize : 0);
      if (need != len) {
        good = false;
      }
      else {
        Pending pe;
        pe.s.entry = hv.entry;
        pe.s.pixel = hv.pixel != 0;
        std::memcpy(pe.s.boolMask, hv.boolMask, sizeof(hv.boolMask));
        pe.s.dkKey = hv.dkKey;
        pe.s.glslSize = hv.glslSize;
        pe.s.zSize = hv.zSize;
        const uint8_t *q = p + head_size;
        pe.input_at = inputs.size();
        for (uint32_t i = 0; i < hv.inputCount; i++, q += sizeof(ShaderVertexInput)) {
          ShaderVertexInput v;
          std::memcpy(&v, q, sizeof(v));
          inputs.push_back(v);
        }
        pe.literal_at = literals.size();
        for (uint32_t i = 0; i < hv.literalCount; i++, q += sizeof(ShaderLiteral)) {
          ShaderLiteral l;
          std::memcpy(&l, q, sizeof(l));
          literals.push_back(l);
        }
        if (v1) {
          // Converting: the GLSL into the new .dat, and the record into the new .idx.
          std::lock_guard<NxMutex> lock(file_mutex_);
          pe.s.zOffset = uint64_t(AppendDatLocked(q, hv.zSize));
          std::vector<uint8_t> payload;
          ShaderRecordHead nh{};
          nh.v1 = hv;
          nh.v1.entry.vertexInputOffset = 0;
          nh.v1.entry.literalOffset = 0;
          nh.zOffset = pe.s.zOffset;
          Put(payload, nh);
          payload.insert(payload.end(), p + head_size, q);  // the inputs and literals as they were
          AppendLocked(kRecordShader, payload);
          if (idx_pending_.size() + dat_pending_.size() >= kBatchBytes)
            SyncLocked();
        }
        else {
          pe.s.zOffset = h.zOffset;
          dat_end_ = std::max<int64_t>(dat_end_, int64_t(h.zOffset + hv.zSize));
        }
        pending.push_back(pe);
      }
    }
    else if (type == kRecordFile && len >= 12) {
      uint64_t fsize;
      uint32_t n;
      std::memcpy(&fsize, p, 8);
      std::memcpy(&n, p + 8, 4);
      if (12 + size_t(n) != len) {
        good = false;
      }
      else {
        scanned_[std::string(reinterpret_cast<const char *>(p + 12), n)] = fsize;
        if (v1) {
          std::lock_guard<NxMutex> lock(file_mutex_);
          AppendLocked(type, std::vector<uint8_t>(p, p + len));
        }
      }
    }
    else if (type == kRecordFailure && len == 8) {
      uint64_t hash;
      std::memcpy(&hash, p, 8);
      failed_.Insert(hash, 0);
      if (v1) {
        std::lock_guard<NxMutex> lock(file_mutex_);
        AppendLocked(type, std::vector<uint8_t>(p, p + len));
      }
    }
    else {
      good = false;
    }
    if (!good)
      break;
    at += 8 + len;
  }
  (void)v1_base;
  inputs_.reserve(inputs.size() + kInputHeadroom);
  literals_.reserve(literals.size() + kLiteralHeadroom);
  inputs_.assign(inputs.begin(), inputs.end());
  literals_.assign(literals.begin(), literals.end());
  // tw2de (2026-10-23): the index made in one go (a sort of the hashes) instead of a hash table node by node; a
  // hash the store has twice keeps its first record, as before.
  std::vector<bool> skip(pending.size(), false);
  {
    std::vector<std::pair<uint64_t, size_t>> order(pending.size());
    for (size_t i = 0; i < pending.size(); ++i)
      order[i] = {pending[i].s.entry.hash, i};
    std::sort(order.begin(), order.end());  // by hash, then by place in the file: the first of a hash leads
    for (size_t i = 0; i < order.size(); ++i)
      skip[order[i].second] = (i > 0 && order[i - 1].first == order[i].first) || index_.Contains(order[i].first);
  }
  std::vector<SortedIndex<StoredShader *>::Item> items;
  items.reserve(index_.Size() + pending.size());
  index_.ForEach([&](uint64_t key, StoredShader *v) { items.push_back({key, v}); });
  for (size_t i = 0; i < pending.size(); ++i) {
    if (skip[i])
      continue;
    Pending &pe = pending[i];
    pe.s.entry.vertexInputOffset = uint32_t(pe.input_at);
    pe.s.entry.literalOffset = uint32_t(pe.literal_at);
    shaders_.push_back(pe.s);
    items.push_back({pe.s.entry.hash, &shaders_.back()});
  }
  index_.Assign(std::move(items));
  if (!v1)
    idx_end_ = kHeaderSize + int64_t(at);
  return true;
}

bool ShaderStore::Load() {
  if (!idx_.Open(idx_path_) || !dat_.Open(dat_path_))
    return false;
  uint8_t head[kHeaderSize];
  if (!idx_.Read(0, head, sizeof(head)) || !HeaderOk(head, kIdxMagic, kVersion) || !dat_.Read(0, head, sizeof(head)) ||
      !HeaderOk(head, kDatMagic, kVersion))
    return false;  // another translation's: Start() over it
  // The records end where .idx.end says (past it: the reserve, or a batch never synced).
  const int64_t end = ReadEnd(idx_path_ + ".end");
  if (end < kHeaderSize || end > idx_.Size())
    return false;
  std::vector<uint8_t> all(size_t(end - kHeaderSize));
  if (!all.empty() && !idx_.Read(kHeaderSize, all.data(), all.size()))
    return false;
  idx_end_ = dat_end_ = kHeaderSize;
  ParseRecords(all.data(), all.size(), false, 0);
  idx_synced_ = idx_end_;
  // Sized to the reserve if it was made with less (or the reserve grew).
  const int64_t reserve = int64_t(std::max(0, int(REXCVAR_GET(tw2de_shader_store_reserve_mb)))) << 20;
  if (dat_.Size() < reserve)
    dat_.SetSize(reserve);
  if (idx_.Size() < reserve / 8)
    idx_.SetSize(reserve / 8);
  return true;
}

// Version 1 (tw2de_shaders.bin, one file, read whole at every start): converted into the .idx and .dat once, and
// deleted; a store of another translation is only deleted.
bool ShaderStore::MigrateV1() {
  NxFile old;
  if (!old.OpenRead(path_))
    return false;
  uint8_t head[kHeaderSize];
  uint32_t version = 0;
  uint64_t tag = 0;
  const bool same = old.Read(0, head, sizeof(head)) && std::memcmp(head, kV1Magic, 8) == 0 &&
                    (std::memcpy(&version, head + 8, 4), version == 1) &&
                    (std::memcpy(&tag, head + 16, 8), tag == TranslatorTag());
  if (same) {
    const int64_t size = old.Size();
    std::vector<uint8_t> all(size_t(std::max<int64_t>(0, size - kHeaderSize)));
    if (!all.empty() && old.Read(kHeaderSize, all.data(), all.size())) {
      ParseRecords(all.data(), all.size(), true, kHeaderSize);
      std::lock_guard<NxMutex> lock(file_mutex_);
      SyncLocked();
    }
  }
  old.Close();
  NxFile::Delete(path_);
  return same;
}

int64_t ShaderStore::AppendDatLocked(const uint8_t *data, size_t size) {
  if (dat_pending_.empty())
    dat_pending_at_ = dat_end_;
  const int64_t at = dat_end_;
  dat_pending_.insert(dat_pending_.end(), data, data + size);
  dat_end_ += int64_t(size);
  return at;
}

void ShaderStore::AppendLocked(uint32_t type, const std::vector<uint8_t> &payload) {
  if (idx_pending_.empty())
    idx_pending_at_ = idx_end_;
  if (unsynced_ == 0)
    batch_started_ = NowNs();
  Put(idx_pending_, type);
  Put(idx_pending_, uint32_t(payload.size()));
  idx_pending_.insert(idx_pending_.end(), payload.begin(), payload.end());
  idx_end_ += 8 + int64_t(payload.size());
  ++unsynced_;
}

// The .idx's end into .idx.end (kept open: one write and a flush).
static bool WriteEnd(NxFile &f, const std::string &path, int64_t end) {
  if (!f.IsOpen() && !f.Open(path, int64_t(sizeof(int64_t))))
    return false;
  return f.Write(0, &end, sizeof(end)) && f.Flush();
}

void ShaderStore::SyncOut(std::unique_lock<NxMutex> &lock, bool force) {
  if (read_only_)
    return;
  if (!force) {
    const bool due = idx_pending_.size() + dat_pending_.size() >= kBatchBytes ||
                     (unsynced_ && NowNs() - batch_started_ >= kBatchSeconds * 1000000000ull);
    if (!due)
      return;
  }
  // Batches in order: the write before this one first. Not waited for under file_mutex_.
  lock.unlock();
  std::lock_guard<NxMutex> order(write_mutex_);
  lock.lock();
  if (read_only_ || (idx_pending_.empty() && dat_pending_.empty() && idx_synced_ == idx_end_))
    return;
  // The batch moves out (no copy), the spare buffers in: the next batch fills them without regrowing.
  dat_inflight_.swap(dat_pending_);
  idx_inflight_.swap(idx_pending_);
  dat_pending_.swap(dat_spare_);
  idx_pending_.swap(idx_spare_);
  if (!force && dat_pending_.capacity() < kBatchBytes + (256u << 10))
    dat_pending_.reserve(kBatchBytes + (256u << 10));  // the pre-pass's, once: then it is reused
  const int64_t idx_at = idx_pending_at_, dat_at = dat_pending_at_, end = idx_end_;
  dat_inflight_at_ = dat_at;
  unsynced_ = 0;
  lock.unlock();
  // Written from dat_inflight_ / idx_inflight_ without the lock: only this thread (write_mutex_) changes them, and
  // ReadGlsl only reads dat_inflight_.
  bool ok = true;
  {
    // The GLSL first, then the records pointing at it, then .idx.end saying so.
    std::lock_guard<NxMutex> card(CardMutex());
    if (!dat_inflight_.empty())
      ok = dat_.Write(dat_at, dat_inflight_.data(), dat_inflight_.size()) && dat_.Flush();
    if (ok && !idx_inflight_.empty())
      ok = idx_.Write(idx_at, idx_inflight_.data(), idx_inflight_.size()) && idx_.Flush();
    if (ok)
      ok = WriteEnd(idx_end_file_, idx_path_ + ".end", end);
  }
  lock.lock();
  if (ok) {
    idx_synced_ = end;
    dat_inflight_.clear();
    idx_inflight_.clear();
    dat_spare_.swap(dat_inflight_);  // cleared, capacity kept
    idx_spare_.swap(idx_inflight_);
  }
  else {
    // The card full or failing: what is in keeps being read; the batch's GLSL stays in memory (dat_inflight_).
    read_only_ = true;
    idx_inflight_.clear();
  }
}

void ShaderStore::SyncLocked() {
  if (read_only_)
    return;
  bool ok = true;
  std::lock_guard<NxMutex> card(CardMutex());
  if (!dat_pending_.empty())
    ok = dat_.Write(dat_pending_at_, dat_pending_.data(), dat_pending_.size()) && dat_.Flush();
  if (ok && !idx_pending_.empty())
    ok = idx_.Write(idx_pending_at_, idx_pending_.data(), idx_pending_.size()) && idx_.Flush();
  if (!ok) {
    read_only_ = true;
    return;
  }
  dat_pending_.clear();
  idx_pending_.clear();
  unsynced_ = 0;
  if (idx_synced_ != idx_end_ && WriteEnd(idx_end_file_, idx_path_ + ".end", idx_end_))
    idx_synced_ = idx_end_;
}

const StoredShader *ShaderStore::Find(uint64_t hash) const {
  std::shared_lock<NxSharedMutex> lock(mutex_);
  StoredShader *const *s = index_.Find(hash);
  return s ? *s : nullptr;
}

const StoredShader *ShaderStore::Add(const TranslatedShader &t) {
  if (const StoredShader *have = Find(t.hash))
    return have;
  thread_local ZSTD_CCtx *cctx = ZSTD_createCCtx();
  std::vector<uint8_t> z(ZSTD_compressBound(t.glsl.size()));
  const size_t zn = ZSTD_compress_usingCDict(cctx, z.data(), z.size(), t.glsl.data(), t.glsl.size(), cdict_);
  if (ZSTD_isError(zn))
    return nullptr;
  z.resize(zn);

  ShaderRecordHead h{};
  h.v1.hash = t.hash;
  h.v1.pixel = t.pixel;
  h.v1.glslSize = uint32_t(t.glsl.size());
  h.v1.zSize = uint32_t(zn);
  h.v1.inputCount = uint32_t(t.inputs.size());
  h.v1.literalCount = uint32_t(t.literals.size());
  std::memcpy(h.v1.boolMask, t.boolMask, sizeof(h.v1.boolMask));
  h.v1.dkKey = Fnv1a64(t.glsl.data(), t.glsl.size());
  h.v1.entry = t.entry;
  h.v1.entry.vertexInputOffset = 0;
  h.v1.entry.literalOffset = 0;

  StoredShader s;
  s.entry = h.v1.entry;
  s.pixel = t.pixel;
  std::memcpy(s.boolMask, t.boolMask, sizeof(s.boolMask));
  s.dkKey = h.v1.dkKey;
  s.glslSize = h.v1.glslSize;
  s.zSize = h.v1.zSize;
  {
    std::unique_lock<NxMutex> lock(file_mutex_);
    if (read_only_) {
      // Not on the card (full): the shader still works this run, from its GLSL kept in memory.
      std::unique_lock<NxSharedMutex> index_lock(mutex_);
      memory_glsl_[t.hash] = t.glsl;
    }
    else {
      h.zOffset = uint64_t(AppendDatLocked(z.data(), z.size()));
      s.zOffset = h.zOffset;
      std::vector<uint8_t> payload;
      payload.reserve(sizeof(h) + t.inputs.size() * sizeof(ShaderVertexInput) + t.literals.size() * sizeof(ShaderLiteral));
      Put(payload, h);
      for (const auto &v : t.inputs)
        Put(payload, v);
      for (const auto &l : t.literals)
        Put(payload, l);
      AppendLocked(kRecordShader, payload);
      // Indexed before the batch may go out (SyncOut lets go of the lock): a Find meanwhile sees it.
      lock.unlock();
      const StoredShader *added = Index(std::move(s), t.inputs.data(), t.literals.data());
      lock.lock();
      SyncOut(lock, false);
      return added;
    }
  }
  return Index(std::move(s), t.inputs.data(), t.literals.data());
}

const StoredShader *ShaderStore::Index(StoredShader &&s, const ShaderVertexInput *inputs,
                                       const ShaderLiteral *literals) {
  std::unique_lock<NxSharedMutex> lock(mutex_);
  if (StoredShader *const *have = index_.Find(s.entry.hash))
    return *have;
  const uint32_t ni = s.entry.vertexInputCount, nl = s.entry.literalCount;
  if (inputs_.size() + ni > inputs_.capacity() || literals_.size() + nl > literals_.capacity())
    return nullptr;  // the arrays are full until the next start (which sizes them from the file)
  s.entry.vertexInputOffset = uint32_t(inputs_.size());
  s.entry.literalOffset = uint32_t(literals_.size());
  inputs_.insert(inputs_.end(), inputs, inputs + ni);
  literals_.insert(literals_.end(), literals, literals + nl);
  shaders_.push_back(std::move(s));
  index_.Set(shaders_.back().entry.hash, &shaders_.back());
  return &shaders_.back();
}

void ShaderStore::AddFailure(uint64_t hash) {
  {
    std::unique_lock<NxSharedMutex> lock(mutex_);
    if (!failed_.Insert(hash, 0))
      return;
  }
  std::vector<uint8_t> payload;
  Put(payload, hash);
  std::unique_lock<NxMutex> lock(file_mutex_);
  AppendLocked(kRecordFailure, payload);
  SyncOut(lock, false);
}

bool ShaderStore::Failed(uint64_t hash) const {
  std::shared_lock<NxSharedMutex> lock(mutex_);
  return failed_.Contains(hash);
}

bool ShaderStore::FileScanned(const std::string &name, uint64_t size) const {
  std::shared_lock<NxSharedMutex> lock(mutex_);
  auto it = scanned_.find(name);
  return it != scanned_.end() && it->second == size;
}

void ShaderStore::MarkFileScanned(const std::string &name, uint64_t size) {
  {
    std::unique_lock<NxSharedMutex> lock(mutex_);
    scanned_[name] = size;
  }
  std::vector<uint8_t> payload;
  Put(payload, size);
  Put(payload, uint32_t(name.size()));
  payload.insert(payload.end(), name.begin(), name.end());
  // After its shaders in the .idx: on the card with them, in the same batch or a later one.
  std::unique_lock<NxMutex> lock(file_mutex_);
  AppendLocked(kRecordFile, payload);
  SyncOut(lock, false);
}

bool ShaderStore::ReadGlsl(const StoredShader &s, std::string &glsl) {
  {
    std::shared_lock<NxSharedMutex> lock(mutex_);
    auto it = memory_glsl_.find(s.entry.hash);
    if (it != memory_glsl_.end()) {
      glsl = it->second;
      return true;
    }
  }
  std::vector<uint8_t> z(s.zSize);
  bool on_card = false;
  if (dat_memory_ && s.zOffset + s.zSize <= dat_memory_size_) {
    // The pre-pass's compiles: the whole .dat in memory, read-only while it is (no lock, no card).
    std::memcpy(z.data(), dat_memory_.get() + s.zOffset, z.size());
  }
  else {
    std::lock_guard<NxMutex> lock(file_mutex_);
    const int64_t at = int64_t(s.zOffset), n = int64_t(z.size());
    if (!dat_pending_.empty() && at >= dat_pending_at_ && at + n <= dat_pending_at_ + int64_t(dat_pending_.size())) {
      std::memcpy(z.data(), dat_pending_.data() + (at - dat_pending_at_), z.size());  // the batch not written yet
    }
    else if (!dat_inflight_.empty() && at >= dat_inflight_at_ &&
             at + n <= dat_inflight_at_ + int64_t(dat_inflight_.size())) {
      std::memcpy(z.data(), dat_inflight_.data() + (at - dat_inflight_at_), z.size());  // the one being written
    }
    else {
      on_card = true;  // written already: read without the store's lock
    }
  }
  if (on_card) {
    std::lock_guard<NxMutex> card(CardMutex());
    if (!dat_.Read(int64_t(s.zOffset), z.data(), z.size()))
      return false;
  }
  thread_local ZSTD_DCtx *dctx = ZSTD_createDCtx();
  glsl.resize(s.glslSize);
  const size_t n = ZSTD_decompress_usingDDict(dctx, glsl.data(), glsl.size(), z.data(), z.size(), ddict_);
  return !ZSTD_isError(n) && n == s.glslSize;
}

void ShaderStore::ForEach(const std::function<void(const StoredShader &)> &fn) const {
  std::shared_lock<NxSharedMutex> lock(mutex_);
  for (const StoredShader &s : shaders_)
    fn(s);
}

size_t ShaderStore::Count() const {
  std::shared_lock<NxSharedMutex> lock(mutex_);
  return shaders_.size();
}

size_t ShaderStore::LoadGlslIntoMemory() {
  Sync();  // every batch on the card first
  std::lock_guard<NxMutex> lock(file_mutex_);
  if (read_only_ || !dat_.IsOpen() || dat_end_ <= 0)
    return 0;
  const size_t size = size_t(dat_end_);
  std::unique_ptr<uint8_t[]> mem(new (std::nothrow) uint8_t[size]);
  if (!mem)
    return 0;
  {
    std::lock_guard<NxMutex> card(CardMutex());
    for (size_t at = 0; at < size; at += 32u << 20) {
      const size_t n = std::min<size_t>(32u << 20, size - at);
      if (!dat_.Read(int64_t(at), mem.get() + at, n))
        return 0;
    }
  }
  dat_memory_ = std::move(mem);
  dat_memory_size_ = size;
  return size;
}

void ShaderStore::ReleaseGlslMemory() {
  std::lock_guard<NxMutex> lock(file_mutex_);
  dat_memory_.reset();
  dat_memory_size_ = 0;
}

void ShaderStore::Sync() {
  std::unique_lock<NxMutex> lock(file_mutex_);
  SyncOut(lock, true);
  // Everything is on the card: the batch buffers go (the game's own adds are one shader at a time).
  std::vector<uint8_t>().swap(dat_spare_);
  std::vector<uint8_t>().swap(idx_spare_);
  if (dat_pending_.empty())
    std::vector<uint8_t>().swap(dat_pending_);
  if (idx_pending_.empty())
    std::vector<uint8_t>().swap(idx_pending_);
}

}  // namespace renut::nxs

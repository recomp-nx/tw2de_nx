// tw2de (2026-10-21): variant_list.h.
#include "renut_engine/gpu/nx_shaders/variant_list.h"

#include <plume_deko3d_spec_defaults.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "renut_engine/gpu/nx_shaders/nx_file.h"

namespace renut::nxs {

std::string VariantEntry::Identity() const {
  char head[40];
  std::snprintf(head, sizeof(head), "%016" PRIX64 " %c ", hash, pixel ? 'p' : 'v');
  std::string s = head;
  for (const auto &[id, value] : values) {
    s += std::to_string(id);
    s += '=';
    s += std::to_string(value);
    s += ';';
  }
  return s;
}

std::string VariantEntry::Line() const {
  char head[48];
  std::snprintf(head, sizeof(head), "%016" PRIX64 " %016" PRIX64 " ", tag, key);
  return head + Identity();
}

bool VariantEntry::Parse(const std::string &line, VariantEntry &out) {
  out = {};
  const char *p = line.c_str();
  char *end = nullptr;
  out.tag = std::strtoull(p, &end, 16);
  if (end == p || *end != ' ')
    return false;
  p = end + 1;
  out.key = std::strtoull(p, &end, 16);
  if (end == p || *end != ' ')
    return false;
  p = end + 1;
  out.hash = std::strtoull(p, &end, 16);
  if (end == p || *end != ' ')
    return false;
  p = end + 1;
  if ((*p != 'p' && *p != 'v') || p[1] != ' ')
    return false;
  out.pixel = *p == 'p';
  p += 2;
  while (*p && *p != '\n' && *p != '\r') {
    const unsigned long id = std::strtoul(p, &end, 10);
    if (end == p || *end != '=')
      return false;
    p = end + 1;
    const unsigned long value = std::strtoul(p, &end, 10);
    if (end == p || *end != ';')
      return false;
    p = end + 1;
    out.values.emplace_back(uint32_t(id), uint32_t(value));
  }
  std::sort(out.values.begin(), out.values.end());
  return true;
}

bool MotionVariant(const VariantEntry &e) {
  for (const auto &v : e.values)
    if (v.first == kVariantWordFx)
      return (v.second & kVariantMotionBit) != 0;
  return false;
}

bool StripMotion(VariantEntry &e) {
  for (auto it = e.values.begin(); it != e.values.end(); ++it) {
    if (it->first != kVariantWordFx)
      continue;
    if (!(it->second & kVariantMotionBit))
      return false;
    it->second &= ~kVariantMotionBit;
    if (!it->second)
      e.values.erase(it);
    e.tag = 0;
    e.key = 0;
    return true;
  }
  return false;
}

uint64_t MotionTwinKey(uint64_t plain_key) {
  uint64_t x = plain_key ^ 0x4D4F54494F4E5631ull;  // a 64-bit mixer (splitmix64's): one key from one key
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

// plume_deko3d.cpp: parseSpecDefaults (plume_deko3d_spec_defaults.h: the constants of the sections the stage compiles,
// sorted by id) and loadVariant (a define for each constant whose value differs, in that order, after the first line).
// Kept the same, or the keys differ from the renderer's.
std::string VariantSource(const std::string &glsl, const std::vector<std::pair<uint32_t, uint32_t>> &values) {
  std::string defines;
  for (const auto &[did, dvalue] : plume::Deko3DParseSpecDefaults(glsl)) {
    for (const auto &[id, value] : values) {
      if (id == did && value != dvalue) {
        char line[64];
        std::snprintf(line, sizeof(line), "#define SPIRV_CROSS_CONSTANT_ID_%u %uu\n", did, value);
        defines += line;
        break;
      }
    }
  }
  const size_t nl = glsl.find('\n');
  if (defines.empty() || nl == std::string::npos)
    return glsl;
  std::string source;
  source.reserve(glsl.size() + defines.size());
  source.append(glsl, 0, nl + 1);
  source += defines;
  source.append(glsl, nl + 1, std::string::npos);
  return source;
}

std::vector<VariantEntry> LoadVariantList(const std::string &path) {
  std::vector<VariantEntry> out;
  NxFile f;
  if (!f.OpenRead(path))
    return out;
  const int64_t size = f.Size();
  if (size <= 0)
    return out;
  std::string text(size_t(size), '\0');
  if (!f.Read(0, text.data(), text.size()))
    return out;
  out.reserve(size_t(std::count(text.begin(), text.end(), '\n')) + 1);
  for (size_t at = 0; at < text.size();) {
    size_t nl = text.find('\n', at);
    if (nl == std::string::npos)
      nl = text.size();
    VariantEntry e;
    if (nl > at && text[at] != '#' && VariantEntry::Parse(text.substr(at, nl - at), e))
      out.push_back(std::move(e));
    at = nl + 1;
  }
  return out;
}

bool SaveVariantList(const std::string &path, const std::vector<VariantEntry> &entries) {
  std::string text;
  text.reserve(entries.size() * 64);
  for (const VariantEntry &e : entries) {
    text += e.Line();
    text += '\n';
  }
  NxFile f;
  if (!f.Open(path, int64_t(text.size())))
    return false;
  return f.SetSize(int64_t(text.size())) && (text.empty() || f.Write(0, text.data(), text.size())) && f.Flush();
}

VariantRecorder &VariantRecorder::Get() {
  static VariantRecorder recorder;
  return recorder;
}

void VariantRecorder::Open(const std::string &path) {
  std::lock_guard<NxMutex> lock(mutex_);
  path_ = path;
  known_.clear();
  for (VariantEntry &e : LoadVariantList(path)) {
    // TW2 (2026-11-16): an older list's motion-vector line counts as its plain form (the pre-pass's start check
    // has rewritten the list without them by now)
    if (StripMotion(e) && e.values.empty())
      continue;
    known_.push_back(e.Identity());
  }
  std::sort(known_.begin(), known_.end());
  known_.erase(std::unique(known_.begin(), known_.end()), known_.end());
  open_ = true;
}

void VariantRecorder::LoadKeys(const std::vector<std::string> &paths, uint64_t tag) {
  std::unordered_map<std::string, uint64_t> keys;
  for (const std::string &path : paths) {
    for (const VariantEntry &e : LoadVariantList(path)) {
      if (e.tag == tag && !MotionVariant(e))  // TW2 (2026-11-16): a twin's key is made (KeyOf), not listed
        keys.emplace(e.Identity(), e.key);  // the first list's line first, as the pre-pass merges them
    }
  }
  std::lock_guard<NxMutex> lock(mutex_);
  keys_.swap(keys);
  keys_tag_ = tag;
}

bool VariantRecorder::KeyOf(uint64_t hash, bool pixel, const std::vector<std::pair<uint32_t, uint32_t>> &values,
                            uint64_t default_key, uint64_t &key) {
  VariantEntry e;
  e.hash = hash;
  e.pixel = pixel;
  e.values = values;
  std::lock_guard<NxMutex> lock(mutex_);
  auto it = keys_.find(e.Identity());
  if (it != keys_.end()) {
    key = it->second;
    return true;
  }
  // TW2 (2026-11-16): a motion-vector twin (FSR 3.1): its key from its plain form's, which is a listed variant's
  // or the shader's default variant's. (One compiled in this session before its plain form was known is found
  // above, under the key plume gave it.)
  if (!StripMotion(e))
    return false;
  if (e.values.empty()) {
    if (!default_key)
      return false;
    key = MotionTwinKey(default_key);
    return true;
  }
  it = keys_.find(e.Identity());
  if (it == keys_.end())
    return false;
  key = MotionTwinKey(it->second);
  return true;
}

void VariantRecorder::Record(const VariantEntry &entry) {
  std::string identity = entry.Identity();
  std::lock_guard<NxMutex> lock(mutex_);
  if (keys_tag_ && entry.tag == keys_tag_)
    keys_.emplace(identity, entry.key);  // tw2de (2026-10-24)
  if (!open_)
    return;
  // TW2 (2026-11-16): the list holds plain variants only. A motion-vector variant (FSR 3.1 on) is recorded as its
  // plain form when the list lacks it, without a key: the next start's pre-pass makes the key and compiles both
  // forms. The twin of a default variant leaves nothing to record.
  VariantEntry plain;
  const VariantEntry *line = &entry;
  if (MotionVariant(entry)) {
    plain = entry;
    StripMotion(plain);
    if (plain.values.empty())
      return;
    identity = plain.Identity();
    line = &plain;
  }
  auto it = std::lower_bound(known_.begin(), known_.end(), identity);
  if (it != known_.end() && *it == identity)
    return;
  known_.insert(it, std::move(identity));
  pending_ += line->Line() + "\n";
  if (kick_) {
    kick_();  // the writer thread calls Flush
    return;
  }
  std::string lines;
  lines.swap(pending_);
  NxFile f;
  if (!f.Open(path_, 0))
    return;
  const int64_t at = std::max<int64_t>(f.Size(), 0);
  if (f.Write(at, lines.data(), lines.size()) && f.Flush())
    ++recorded_;
}

void VariantRecorder::Flush() {
  std::string lines;
  {
    std::lock_guard<NxMutex> lock(mutex_);
    if (!open_ || pending_.empty())
      return;
    lines.swap(pending_);
  }
  // One writer (the renderer's card writer thread): the appends stay in order.
  NxFile f;
  if (!f.Open(path_, 0))
    return;
  const int64_t at = std::max<int64_t>(f.Size(), 0);
  if (f.Write(at, lines.data(), lines.size()) && f.Flush()) {
    std::lock_guard<NxMutex> lock(mutex_);
    recorded_ += size_t(std::count(lines.begin(), lines.end(), '\n'));
  }
}

}  // namespace renut::nxs

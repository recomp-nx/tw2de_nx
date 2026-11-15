// tw2de (2026-10-23): late_spec.h.
#include "renut_engine/gpu/nx_shaders/late_spec.h"

#include <cctype>
#include <cstring>
#include <unordered_map>

namespace renut::nxs {

namespace {

constexpr uint32_t kPackMagic = 0x534C5846u;  // "FXLS"
constexpr uint32_t kPackVersion = 1;
constexpr uint32_t kEarlyZ = 909, kGprCap = 910;

int Slot(uint32_t id) {
  if (id >= 900 && id < 916)
    return int(id - 900);
  if (id >= 1000 && id < 1000 + (kLateSpecSlots - 16))
    return int(16 + (id - 1000));
  return -1;
}

bool ReadUint(const std::string &s, size_t &i, uint32_t &v) {
  const size_t start = i;
  uint64_t x = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && x <= 0xFFFFFFFFull)
    x = x * 10 + uint64_t(s[i++] - '0');
  v = uint32_t(x);
  return i > start && x <= 0xFFFFFFFFull;
}

bool IsIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

}  // namespace

bool LateSpecBase(const std::string &glsl, std::string &base, std::vector<std::pair<uint32_t, uint32_t>> &slot_defaults) {
  static constexpr char kDecl[] = "const uint ";
  static constexpr char kId[] = "SPIRV_CROSS_CONSTANT_ID_";
  static constexpr char kIfndef[] = "#ifndef SPIRV_CROSS_CONSTANT_ID_";
  static constexpr char kDefine[] = "#define SPIRV_CROSS_CONSTANT_ID_";
  slot_defaults.clear();

  // The defaults: "#ifndef SPIRV_CROSS_CONSTANT_ID_<id>\n#define SPIRV_CROSS_CONSTANT_ID_<id> <value>u".
  std::unordered_map<uint32_t, uint32_t> defaults;
  for (size_t at = glsl.find(kIfndef); at != std::string::npos; at = glsl.find(kIfndef, at + 1)) {
    size_t i = at + sizeof(kIfndef) - 1;
    uint32_t id = 0, v = 0;
    if (!ReadUint(glsl, i, id) || i >= glsl.size() || glsl[i] != '\n')
      continue;
    ++i;
    if (glsl.compare(i, sizeof(kDefine) - 1, kDefine) != 0)
      continue;
    i += sizeof(kDefine) - 1;
    uint32_t id2 = 0;
    if (!ReadUint(glsl, i, id2) || id2 != id || i >= glsl.size() || glsl[i] != ' ')
      continue;
    ++i;
    if (!ReadUint(glsl, i, v) || i >= glsl.size() || glsl[i] != 'u')
      continue;
    defaults[id] = v;
  }

  // The consts become reads of the buffer.
  std::string out;
  out.reserve(glsl.size() + 256);
  std::vector<uint32_t> done;
  size_t at = 0;
  for (size_t k = glsl.find(kDecl); k != std::string::npos; k = glsl.find(kDecl, k + 1)) {
    if (k > 0 && IsIdent(glsl[k - 1]))
      continue;
    size_t i = k + sizeof(kDecl) - 1;
    const size_t name_at = i;
    while (i < glsl.size() && IsIdent(glsl[i]))
      ++i;
    const std::string name = glsl.substr(name_at, i - name_at);
    if (name.empty() || glsl.compare(i, 3, " = ") != 0)
      continue;
    i += 3;
    if (glsl.compare(i, sizeof(kId) - 1, kId) != 0)
      continue;
    i += sizeof(kId) - 1;
    uint32_t id = 0;
    if (!ReadUint(glsl, i, id) || i >= glsl.size() || glsl[i] != ';')
      continue;
    ++i;
    if (id == kEarlyZ)
      continue;
    const int slot = Slot(id);
    auto d = defaults.find(id);
    if (slot < 0 || d == defaults.end())
      return false;  // a constant the buffer has no slot for, or without its default
    out.append(glsl, at, k - at);
    out += "#define " + name + " (dk_spec.v[" + std::to_string(slot >> 2) + "]." + "xyzw"[slot & 3] + ")";
    at = i;
    slot_defaults.emplace_back(uint32_t(slot), d->second);
    done.push_back(id);
  }
  out.append(glsl, at, std::string::npos);
  if (done.empty())
    return false;  // nothing to specialise

  // Every other use of a constant's name is a #ifndef / #define of it, a comment, or constant 909's.
  size_t line = 0;
  while (line < out.size()) {
    size_t end = out.find('\n', line);
    if (end == std::string::npos)
      end = out.size();
    size_t p = out.find(kId, line);
    if (p != std::string::npos && p < end) {
      size_t s = line;
      while (s < end && (out[s] == ' ' || out[s] == '\t'))
        ++s;
      const bool ok = out.compare(s, 2, "//") == 0 || out.compare(s, sizeof(kIfndef) - 1, kIfndef) == 0 ||
                      out.compare(s, sizeof(kDefine) - 1, kDefine) == 0;
      if (!ok) {
        // a use of 909 only (anything else: not late-specialisable)
        for (; p != std::string::npos && p < end; p = out.find(kId, p + 1)) {
          size_t i = p + sizeof(kId) - 1;
          uint32_t id = 0;
          if (!ReadUint(out, i, id) || id != kEarlyZ)
            return false;
        }
      }
    }
    line = end + 1;
  }

  // The buffer, after the #extension lines (none may follow a declaration), else after #version.
  size_t insert = std::string::npos;
  for (size_t e = out.find("#extension"); e != std::string::npos; e = out.find("#extension", e + 1)) {
    if (e == 0 || out[e - 1] == '\n') {
      const size_t nl = out.find('\n', e);
      insert = nl == std::string::npos ? out.size() : nl + 1;
    }
  }
  if (insert == std::string::npos) {
    const size_t nl = out.find('\n');
    insert = nl == std::string::npos ? out.size() : nl + 1;
  }
  out.insert(insert, "layout(binding = " + std::to_string(kLateSpecBinding) +
                         ", std140) uniform DkSpec { uvec4 v[" + std::to_string(kLateSpecSlots / 4) + "]; } dk_spec;\n");
  base.swap(out);
  return true;
}

void LateSpecPack(const std::vector<std::pair<uint32_t, uint32_t>> &slot_defaults, const std::vector<uint8_t> &front_end,
                  std::vector<uint8_t> &out) {
  const uint32_t head[3] = {kPackMagic, kPackVersion, uint32_t(slot_defaults.size())};
  out.resize(sizeof(head) + slot_defaults.size() * 8 + front_end.size());
  uint8_t *p = out.data();
  std::memcpy(p, head, sizeof(head));
  p += sizeof(head);
  for (const auto &d : slot_defaults) {
    std::memcpy(p, &d.first, 4);
    std::memcpy(p + 4, &d.second, 4);
    p += 8;
  }
  if (!front_end.empty())
    std::memcpy(p, front_end.data(), front_end.size());
}

bool LateSpecPrepare(const std::vector<uint8_t> &blob, const std::vector<std::pair<uint32_t, uint32_t>> &values,
                     LateSpecInputs &in) {
  uint32_t head[3];
  if (blob.size() < sizeof(head))
    return false;
  std::memcpy(head, blob.data(), sizeof(head));
  if (head[0] != kPackMagic || head[1] != kPackVersion || sizeof(head) + size_t(head[2]) * 8 > blob.size())
    return false;
  in.slots.assign(kLateSpecSlots, 0u);
  in.early_z = false;
  in.gpr_cap = 0;
  uint64_t known[kLateSpecSlots / 64] = {};
  const uint8_t *p = blob.data() + sizeof(head);
  for (uint32_t i = 0; i < head[2]; ++i, p += 8) {
    uint32_t slot, v;
    std::memcpy(&slot, p, 4);
    std::memcpy(&v, p + 4, 4);
    if (slot >= kLateSpecSlots)
      return false;
    in.slots[slot] = v;
    known[slot / 64] |= 1ull << (slot % 64);
  }
  for (const auto &[id, v] : values) {
    if (id == kEarlyZ) {
      in.early_z = true;  // defined at all (plume defines it only when it differs from 0): the GLSL's DK_EARLY_Z 1
      continue;
    }
    if (id == kGprCap) {
      in.gpr_cap = v ? 32 : 0;
      continue;
    }
    const int slot = Slot(id);
    if (slot < 0 || !(known[slot / 64] & (1ull << (slot % 64))))
      return false;  // a constant the base form does not read from the buffer
    in.slots[size_t(slot)] = v;
  }
  in.front_end = p;
  in.front_end_size = blob.size() - size_t(p - blob.data());
  return in.front_end_size > 0;
}

}  // namespace renut::nxs

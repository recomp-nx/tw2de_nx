// tw2de (2026-11-10, tw2de_diag_wdb): the game's database lookups by name, logged (the first 64 that find nothing, and of the names pc* / m7*), for the
// prologue's battle: "battle chara spec <pc100> not found" (DatabaseBattleCharaSpec.cpp/39) for every character.
// sub_8263EE08(manager, key: 16-byte name, all) looks the name up in each database the manager holds (the bit set at
// +0x48.., the databases at +8..) and in the one at +0x98, through each database's vtable +4 (find(key, out)).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <string>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(tw2de_diag_wdb, false, "Game/Diagnostics",
                    "Log the game's database lookups by name (the first 64 that find nothing, and of the names pc* / m7*): the key, the databases tried, the result")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8263EE08);

namespace {
uint32_t Be32(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
std::string Name(const uint8_t *base, uint32_t ea) {
  std::string s;
  for (int i = 0; i < 16 && ea; ++i) {
    const char c = char(base[ea + i]);
    if (!c)
      break;
    s += (c >= 32 && c < 127) ? c : '?';
  }
  return s;
}
}  // namespace

extern "C" REX_FUNC(sub_8263EE08) {
  static const bool on = REXCVAR_GET(tw2de_diag_wdb);
  if (!on) {
    __imp__sub_8263EE08(ctx, base);
    return;
  }
  const uint32_t mgr = ctx.r3.u32, key = ctx.r4.u32, all = ctx.r5.u32, lr = uint32_t(ctx.lr);
  const std::string name = Name(base, key);
  std::string dbs;
  if (mgr) {
    const uint32_t main = Be32(base, mgr + 0x98);
    if (main) {
      const uint32_t vt = Be32(base, main);
      dbs += fmt::format(" main {:08X} vt {:08X} find {:08X}", main, vt, vt ? Be32(base, vt + 4) : 0);
    }
    for (uint32_t i = 0; i < 16; ++i) {
      const uint32_t bits = Be32(base, mgr + 8 + ((i >> 5) + 0x10) * 4);
      if (!(bits & (1u << (i & 31))))
        continue;
      const uint32_t entry = Be32(base, mgr + 8 + i * 4);
      const uint32_t db = entry ? Be32(base, entry + 0x98) : 0;
      dbs += fmt::format(" [{}] {:08X}/{:08X}", i, entry, db);
    }
  }
  __imp__sub_8263EE08(ctx, base);
  // the first 64 that found nothing, and the first 64 of the battle characters' names (pc*, m7*)
  static std::atomic<int> n_failed{0}, n_chara{0};
  const bool chara = name.rfind("pc", 0) == 0 || name.rfind("m7", 0) == 0;
  if ((ctx.r3.u32 == 0 && n_failed.fetch_add(1) < 64) || (chara && n_chara.fetch_add(1) < 64))
    REXLOG_INFO("[wdb] find <{}> in {:08X} (all {}) from {:08X} -> {:08X};{}", name, mgr, all, lr, ctx.r3.u32, dbs);
}

// tw2de (2026-11-10, LR): the lookup sub_8263EE08 (FF XIII-2 824E8718: its one caller's call, shape 0.65) and
// the binary search sub_822F9550 (FF XIII-2 82251758: the same search, LR's build adds a stack guard and takes the
// count's top 16 bits at +8 for its empty test; the count read here is +4 as in both).
// sub_822F9550(table, key, out): the binary search (the 16-byte name as two big-endian 64-bit words, the records at
// +0x10 + 32 * i, +4 their count). When a battle character's name (pc*, m7*) is not found: the key's 16 bytes, the
// count and the table's first and last names (the first 16 such).
REX_EXTERN(__imp__sub_822F9550);
extern "C" REX_FUNC(sub_822F9550) {
  static const bool on = REXCVAR_GET(tw2de_diag_wdb);
  const uint32_t table = ctx.r3.u32, key = ctx.r4.u32;
  __imp__sub_822F9550(ctx, base);
  if (!on || ctx.r3.u32 || !key || !table)
    return;
  const std::string name = Name(base, key);
  if (name.rfind("pc", 0) != 0 && name.rfind("m7", 0) != 0)
    return;
  static std::atomic<int> n{0};
  if (n.fetch_add(1) >= 16)
    return;
  std::string hex;
  for (int i = 0; i < 16; ++i)
    hex += fmt::format("{:02X}", base[key + i]);
  const uint32_t count = Be32(base, table + 4);
  std::string names;
  for (uint32_t i = 0; i < count && i < 4; ++i)
    names += " " + Name(base, table + 0x10 + 32 * i);
  if (count > 4)
    names += " ... " + Name(base, table + 0x10 + 32 * (count - 1));
  REXLOG_INFO("[wdb] search <{}> key {} in table {:08X}: {} records (+8 {:08X}):{}", name, hex, table, count,
              Be32(base, table + 8), names);
  if (n.load() == 1) {  // the whole table once: every name with its first word in hex
    std::string all;
    for (uint32_t i = 0; i < count && i < 512; ++i)
      all += fmt::format(" {}={}", i, Name(base, table + 0x10 + 32 * i));
    REXLOG_INFO("[wdb] table {:08X} names:{}", table, all);
  }
}

// tw2de (2026-11-11): the menus' network and store entries left out (title menu, settings, the New Game prompts).
//
// tw2de_diag_text_keys: every text key the game looks up (sub_8270F178(?, key), the one lookup by key under
// sub_8272BC28 and the others), logged once for each key and call chain: the key and the guest return addresses
// (lr, then each frame's saved lr at [back chain - 8]).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <fmt/format.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

REXCVAR_DEFINE_BOOL(tw2de_diag_text_keys, false, "Game/Diagnostics",
                    "Log each text key the game looks up, with the guest call chain (once for each)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8270F178);  // a text by key: (?, key)

namespace {
inline uint32_t Load32(const uint8_t *base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, base + ea, 4);
  return __builtin_bswap32(v);
}
bool GuestPointer(uint32_t ea) { return ea >= 0x40000000u && ea < 0xA0000000u; }
bool CodePointer(uint32_t ea) { return ea >= 0x822C0000u && ea < 0x82F00000u; }

void NoteKey(const uint8_t *base, uint32_t key, uint32_t lr, uint32_t sp) {
  if (!key || !(GuestPointer(key) || CodePointer(key) || (key >= 0x82000000u && key < 0x84000000u)))
    return;
  const char *k = reinterpret_cast<const char *>(base + key);
  const size_t n = strnlen(k, 48);
  std::string line = fmt::format("[keys] {} <- {:08X}", std::string(k, n), lr);
  uint32_t f = sp;
  for (int i = 0; i < 6; ++i) {
    if (!GuestPointer(f) && !(f >= 0x70000000u))
      break;
    const uint32_t up = Load32(base, f);
    if (!up || up <= f || up - f > 0x100000u)
      break;
    const uint32_t ra = Load32(base, up - 8);
    if (!CodePointer(ra))
      break;
    line += fmt::format(" < {:08X}", ra);
    f = up;
  }
  static std::mutex m;
  static std::unordered_set<std::string> seen;
  std::lock_guard<std::mutex> lock(m);
  if (seen.size() >= 4000 || !seen.insert(line).second)
    return;
  REXLOG_INFO("{}", line);
}
}  // namespace

namespace tw2de {
// sub_8272BC28's hook (tw2de_switch_texts.cpp): its key and caller, before the lookup.
void NoteTextLookup(const uint8_t *base, uint32_t key, uint32_t lr, uint32_t sp) {
  static const bool diag = REXCVAR_GET(tw2de_diag_text_keys);
  if (diag)
    NoteKey(base, key, lr, sp);
}
}  // namespace tw2de

extern "C" REX_FUNC(sub_8270F178) {
  static const bool diag = REXCVAR_GET(tw2de_diag_text_keys);
  if (diag)
    NoteKey(base, ctx.r4.u32, uint32_t(ctx.lr), ctx.r1.u32);
  __imp__sub_8270F178(ctx, base);
}

// tw2de_diag_script_calls: the event scripts' method calls (the VM's method resolution sub_829A42B0(class
// name?, index, &class, vm) under its invoke operations), the first three of each class.method, in order: class name
// [class+0x20], method name: the class's constant pool ([class+0x18], 16 bytes an entry, tag 1 a string at +12) at the
// method's +2, a native one with its code at [method+12].
REXCVAR_DEFINE_BOOL(tw2de_diag_script_calls, false, "Game/Diagnostics",
                    "Log the event scripts' method calls (the first three of each)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tw2de_diag_script_dump: at the scripts' static calls (sub_829A9110(vm, class, pc): pc at the invoke byte, the
// pool index at pc+1..2, the operand stack at [vm+0x958], 8 bytes an entry, the value at +4) whose top argument is a
// text "$sys_sns..." or a tips window's id: the bytecode around pc.
REXCVAR_DEFINE_BOOL(tw2de_diag_script_dump, false, "Game/Diagnostics",
                    "Log the script bytecode around the Outerworld prompts' calls")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_829A42B0);
REX_EXTERN(__imp__sub_829A9110);
thread_local uint32_t t_invoke_pc = 0;

namespace {
std::string GuestString(const uint8_t *base, uint32_t ea) {
  if (!GuestPointer(ea) && !(ea >= 0x82000000u && ea < 0x84000000u))
    return "?";
  const char *s = reinterpret_cast<const char *>(base + ea);
  return std::string(s, strnlen(s, 64));
}
}  // namespace

extern "C" REX_FUNC(sub_829A42B0) {
  static const bool diag = REXCVAR_GET(tw2de_diag_script_calls);
  const uint32_t out = ctx.r5.u32, vm = ctx.r6.u32;
  __imp__sub_829A42B0(ctx, base);
  if (!diag || !ctx.r3.u32 || !out)
    return;
  const uint32_t method = ctx.r3.u32, cls = Load32(base, out);
  if (!GuestPointer(cls))
    return;
  const uint32_t index = uint32_t(base[method + 2]) << 8 | base[method + 3];
  const uint32_t count = uint32_t(base[cls + 8]) << 8 | base[cls + 9];
  std::string name = "?";
  if (index < count) {
    const uint32_t entry = Load32(base, cls + 0x18) + index * 16;
    if (base[entry] == 1)
      name = GuestString(base, Load32(base, entry + 12));
  }
  static const bool dump = REXCVAR_GET(tw2de_diag_script_dump);
  if (dump && (name == "getResourceString" || name == "createTipsWindowById") && t_invoke_pc) {
    const uint32_t sp = Load32(base, vm + 0x958), pc = t_invoke_pc;
    const std::string arg = GuestPointer(sp) ? GuestString(base, Load32(base, sp + 4)) : std::string("?");
    if (arg.rfind("$sys_sns", 0) == 0 || name == "createTipsWindowById") {
      std::string hex;
      for (uint32_t a = pc - 256; a < pc + 256; ++a)
        hex += fmt::format("{:02X}{}", base[a], a + 1 == pc ? "|" : "");
      REXLOG_INFO("[script dump] {} \"{}\" pc {:08X} class {}: {}", name, arg, pc,
                  GuestString(base, Load32(base, cls + 0x20)), hex);
    }
  }
  const std::string line =
      fmt::format("[script] {}.{}{}", GuestString(base, Load32(base, cls + 0x20)), name,
                  Load32(base, method + 12) ? fmt::format(" native {:08X}", Load32(base, method + 12)) : std::string());
  static std::mutex m;
  static std::unordered_map<std::string, int> seen;
  static uint32_t lines = 0;
  std::lock_guard<std::mutex> lock(m);
  if (lines >= 30000 || ++seen[line] > 3)
    return;
  ++lines;
  REXLOG_INFO("{}", line);
}


// tw2de_no_outerworld_prompts: the game start's Outerworld steps left out. The start-up script's state machine
// (its state in a static field, pool 01F2), in its bytecode (tw2de_085.log, the same for New Game and Continue):
//   if (isEnablePremium()) state = 1;
//   else { tips = createTipsWindowById("atcw_fl034"); state = 24; }             <- the networking Primer
//   24: when the tips window is closed: destroy it, mark it seen;
//       if (checkConfigBoolean(16)) state = 30;                                  <- Outerworld already on
//       else { showMessageWindow("$sys_sns000", Yes/No); state = 25; }           <- "Do you want to enable..."
//   25: Yes: config 16 on, state 30; No: showMessageWindow("$sys_sns001"); ...   <- "Change Outerworld services..."
// At the call creating the Primer (ldc_w 0204 "atcw_fl034"; invokestatic 0200; putstatic 0205; bipush 24; putstatic
// 01F2) its argument is taken off the operand stack and the script goes on at state 24's own "state = 30" (bipush 30;
// putstatic 01F2; goto, 0x30 bytes on): no Primer, no question, no notice, config 16 (Outerworld) left off. The bytes
// are checked first; anything else runs as before.
REXCVAR_DEFINE_BOOL(tw2de_no_outerworld_prompts, true, "Game/Enhancements",
                    "At the game start, no networking Primer and no \"enable Outerworld services\" question (they stay off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" REX_FUNC(sub_829A9110) {
  const uint32_t vm = ctx.r3.u32, pc = ctx.r5.u32;
  t_invoke_pc = pc;
  static const bool skip = REXCVAR_GET(tw2de_no_outerworld_prompts);
  static constexpr uint8_t kCall[] = {0x13, 0x02, 0x04, 0xB8, 0x02, 0x00, 0xB3, 0x02, 0x05, 0x10, 0x18, 0xB3, 0x01, 0xF2};
  static constexpr uint8_t kState30[] = {0x10, 0x1E, 0xB3, 0x01, 0xF2, 0xA7};
  if (skip && base[pc] == 0xB8 && base[pc + 1] == 0x02 && base[pc + 2] == 0x00 &&
      std::memcmp(base + pc - 3, kCall, sizeof(kCall)) == 0 &&
      std::memcmp(base + pc + 0x30, kState30, sizeof(kState30)) == 0) {
    const uint32_t sp = Load32(base, vm + 0x958);
    if (GuestPointer(sp) && GuestString(base, Load32(base, sp + 4)) == "atcw_fl034") {
      const uint32_t up = __builtin_bswap32(sp + 8);
      std::memcpy(base + vm + 0x958, &up, 4);
      ctx.r3.u64 = pc + 0x30;
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true))
        REXLOG_INFO("[menus] the game start's Outerworld Primer and question left out (tw2de_no_outerworld_prompts)");
      return;
    }
  }
  __imp__sub_829A9110(ctx, base);
}

// The title menu (sub_827F66B8 builds it; sub_827F5490(menu, 1) shows its main list's entries: the list is the 1st
// child of the menu's root ([[menu+0x44]+0x14], linked by +0x20), 8 entries laid out from the top in the order of the
// table at 0x8212DFF0 (0, 6, 1, 2, 4, 5, 3, 7), each one hidden, and skipped by the layout and the cursor, when its
// +0xBC has bit 2 (its vfunc +0x18, sub_82308868, sets it). Seen (tw2de_086.log): #0 New Game, #1 Continue,
// #2 Settings, #4 Install, #5 Downloadable Content shown, #3 #6 #7 hidden. Install and DLC are the last two shown,
// so hiding them after the list is laid out moves nothing else.
REXCVAR_DEFINE_BOOL(tw2de_title_menu_trim, true, "Game/Enhancements",
                    "The title menu shows no Install and no Downloadable Content")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_827F5490);

extern "C" REX_FUNC(sub_827F5490) {
  const uint32_t menu = ctx.r3.u32, level = ctx.r4.u32;
  __imp__sub_827F5490(ctx, base);
  static const bool trim = REXCVAR_GET(tw2de_title_menu_trim);
  if (!trim || level != 1)
    return;
  const uint32_t root = Load32(base, menu + 0x44);
  if (!GuestPointer(root))
    return;
  uint32_t list = Load32(base, root + 0x14);
  for (uint32_t i = 0; i < level && GuestPointer(list); ++i)
    list = Load32(base, list + 0x20);
  if (!GuestPointer(list))
    return;
  uint32_t entries[8] = {}, n = 0;
  for (uint32_t e = Load32(base, list + 0x14); GuestPointer(e) && n < 8; e = Load32(base, e + 0x20))
    entries[n++] = e;
  if (n != 8)
    return;
  for (const uint32_t i : {4u, 5u}) {
    const uint32_t flags = Load32(base, entries[i] + 0xBC) | 2u;
    const uint32_t be = __builtin_bswap32(flags);
    std::memcpy(base + entries[i] + 0xBC, &be, 4);
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    REXLOG_INFO("[menus] title menu: Install and Downloadable Content hidden (tw2de_title_menu_trim)");
}

// The settings menu's entries: sub_827903A8(menu) takes the candidates (the table at 0x8211C530) the game allows and
// keeps them in order, the entry's id at menu+4*k, its current value (sub_827319C8(id), a value, nothing allocated) at
// menu+0x5C+4*k, their count at menu+0xB8. Each id's label is "$f_con_NN", NN from the table at 0x8211B820: id 10
// "Outerworld Services" (48), 12 "Outerworld Characters" (55), 13 "Link with Facebook" (59), 14 "Link with Twitter"
// (62). Those four are taken out of the list (the rest moved up), so they are neither shown nor changed: Outerworld
// stays as it is (off).
REXCVAR_DEFINE_BOOL(tw2de_settings_menu_trim, true, "Game/Enhancements",
                    "The settings menu shows no Outerworld Services, Outerworld Characters, Link with Facebook and Link "
                    "with Twitter")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_827903A8);

extern "C" REX_FUNC(sub_827903A8) {
  const uint32_t menu = ctx.r3.u32;
  __imp__sub_827903A8(ctx, base);
  static const bool trim = REXCVAR_GET(tw2de_settings_menu_trim);
  if (!trim || !GuestPointer(menu))
    return;
  const uint32_t count = Load32(base, menu + 0xB8);
  if (count > 0x16)
    return;
  auto store = [&](uint32_t ea, uint32_t v) {
    const uint32_t be = __builtin_bswap32(v);
    std::memcpy(base + ea, &be, 4);
  };
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = Load32(base, menu + 4 * i), value = Load32(base, menu + 0x5C + 4 * i);
    if (id == 10 || id == 12 || id == 13 || id == 14)
      continue;
    store(menu + 4 * kept, id);
    store(menu + 0x5C + 4 * kept, value);
    ++kept;
  }
  store(menu + 0xB8, kept);
  static std::atomic<bool> logged{false};
  if (kept != count && !logged.exchange(true))
    REXLOG_INFO("[menus] settings: the Outerworld and Facebook/Twitter entries left out, {} of {} (tw2de_settings_menu_trim)",
                kept, count);
}

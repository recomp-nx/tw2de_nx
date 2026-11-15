// tw2de (2026-11-01, tw2de_diag_touch_off): keeps chosen collision bodies of the field switched off, for
// diagnostics.
//
// The field's scripts switch collision bodies on and off with sub_823ABEC8(body, on): it logs
// "[RIGIDBODY_TOUCH_PLAN] <name / ON|OFF>" (the name from the body's +4 interface, its slot 16) and passes `on` to the
// body (its slot 80). The Palumpolum boat docks (zone z008) raise walls, bx_col_*, while the boat is away ("col on 1":
// bx_col_0141 and bx_col_0152 ON) and drop them when it arrives (the same two OFF). With the cvar the bodies whose names
// start with one of its entries are always given OFF: the walls never come up. If the player can then walk onto the
// boat, one of those walls stays solid after its OFF; if not, they are not what stops the player.
// Empty (the default): sub_823ABEC8 runs as before, a cvar read more.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(tw2de_diag_touch_off, "", "Game/Diagnostics",
                      "Diagnostic: field collision bodies kept off, by name or name start (\"bx_col_\": the boat docks' "
                      "walls)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_823ABEC8);  // (body, on)

namespace {
uint32_t Load32(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}

std::vector<std::string> Entries() {
  std::vector<std::string> list;
  const std::string all = REXCVAR_GET(tw2de_diag_touch_off);
  size_t start = 0;
  while (start <= all.size()) {
    size_t end = all.find(',', start);
    if (end == std::string::npos)
      end = all.size();
    std::string e = all.substr(start, end - start);
    while (!e.empty() && e.front() == ' ')
      e.erase(e.begin());
    while (!e.empty() && e.back() == ' ')
      e.pop_back();
    if (!e.empty())
      list.push_back(e);
    start = end + 1;
  }
  if (!list.empty())
    REXLOG_INFO("[touch] kept off: {}", all);
  return list;
}

// The body's name, as the game's own log gets it: (body + 4)'s slot 16.
std::string Name(PPCContext &ctx, uint8_t *base, uint32_t body) {
  const uint32_t iface = body + 4;
  const uint32_t fn = Load32(base, Load32(base, iface) + 16);
  PPCFunc *func = rex::runtime::ResolveIndirectFunction(fn);
  if (!func)
    return {};
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  ctx.r3.u64 = iface;
  func(ctx, base);
  const uint32_t name = ctx.r3.u32;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.lr = lr;
  if (!name)
    return {};
  const char *s = reinterpret_cast<const char *>(base + name);
  return std::string(s, strnlen(s, 128));
}
}  // namespace

extern "C" REX_FUNC(sub_823ABEC8) {
  static const std::vector<std::string> entries = Entries();
  if (!entries.empty() && ctx.r3.u32 && ctx.r4.u32) {
    const std::string name = Name(ctx, base, ctx.r3.u32);
    for (const std::string &e : entries) {
      if (name.compare(0, e.size(), e) == 0) {
        REXLOG_INFO("[touch] {} ON given as OFF", name);
        ctx.r4.u64 = 0;
        break;
      }
    }
  }
  __imp__sub_823ABEC8(ctx, base);
}

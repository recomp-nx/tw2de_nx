// tw2de (2026-10-25, tw2de_skip_boot_prompts): "Press any button" after the publisher logo answered at once.
//
// tw2de (2026-11-10, LR): LR's start-up flow is sub_822F0EA8 (FF XIII-2's sub_82259120: shape 0.75, the same
// calls in the same order, the prompt's at 0x822F10AC-0x822F1158); the addresses below are LR's (the call sites' returns
// and the message functions renamed from FF XIII-2's one for one; playSE sub_827183B8: the same type-indexed name lookup).
// tw2de (2026-11-10): FF XIII-2's start-up flow, sub_82259120, has FF XIII's (sub_82200F80) prompt step for step
// (the calls in the same order, tools/match_functions.py for each function):
//   "Press any button": the message made (sub_82702670, return 0x822F10B0), opened (sub_826FAC20, 0x822F10C0), its
//     mode (sub_826FA870, 0x822F10CC); one keystroke from any controller through the XInputGetKeystroke wrapper
//     sub_82D35BF8: once (0x822F10DC), after Sleep(500) (sub_82D35B18, 0x822F10E4) again (0x822F10F4), then every
//     millisecond (0x822F1114) until one comes; then closed (sub_826FC110, 0x822F1128), the waits for its closing
//     animation (sub_826F9490, 0x822F1130 and 0x822F1148: 0 = done) and destroyed (sub_826FC140, 0x822593BC).
//     Here the keystroke calls get an A press of player 1 (user 0) at once, the 500 ms sleep is skipped, and the
//     message is made and destroyed as before but never opened, closed or waited for (FF XIII 2026-10-25: its closing
//     played the confirm sound); the UI sounds its creation plays (FF XIII: GuiManager::playSE type 47; FF XIII-2's
//     playSE sub_827183B8 from the same place, type 48) muted on that thread while it is made.
//   FF XIII's storage device notice ("chooseDeviceFirst", its message dialog sub_8245FFE0 from the flow) has no call in
//   FF XIII-2's flow (its dialog, sub_8265A9D0, is called only from sub_8250CB88): nothing to skip there.
// Only these call sites change: the same functions called from anywhere else run as before.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <initializer_list>
#include <cstdint>

REXCVAR_DEFINE_BOOL(tw2de_skip_boot_prompts, true, "Game/Enhancements",
                    "Skip \"Press any button\" after the publisher logo (player 1 is taken)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82D35BF8);  // XInputGetKeystroke wrapper
REX_EXTERN(__imp__sub_82D35B18);  // Sleep wrapper
REX_EXTERN(__imp__sub_826FAC20);  // a message: open
REX_EXTERN(__imp__sub_826FA870);  // a message: its mode
REX_EXTERN(__imp__sub_826FC110);  // a message: close
REX_EXTERN(__imp__sub_826F9490);  // a message: still animating

namespace {
bool On() {
  static const bool on = REXCVAR_GET(tw2de_skip_boot_prompts);
  return on;
}
void Store16(uint8_t *base, uint32_t ea, uint16_t v) { *reinterpret_cast<uint16_t *>(base + ea) = __builtin_bswap16(v); }
}  // namespace

extern "C" REX_FUNC(sub_82D35BF8) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (On() && (lr == 0x822F10DCu || lr == 0x822F10F4u || lr == 0x822F1114u) && ctx.r5.u32) {
    const uint32_t keystroke = ctx.r5.u32;  // XINPUT_KEYSTROKE: VirtualKey, Unicode, Flags (u16), UserIndex, HidCode
    Store16(base, keystroke + 0, 0x5800);   // VK_PAD_A
    Store16(base, keystroke + 2, 0);
    Store16(base, keystroke + 4, 0x0001);   // XINPUT_KEYSTROKE_KEYDOWN
    base[keystroke + 6] = 0;                // player 1
    base[keystroke + 7] = 0;
    ctx.r3.u64 = 0;                         // ERROR_SUCCESS
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      REXLOG_INFO("[boot] \"Press any button\" answered: player 1 (tw2de_skip_boot_prompts)");
    return;
  }
  __imp__sub_82D35BF8(ctx, base);
}

extern "C" REX_FUNC(sub_82D35B18) {
  if (On() && uint32_t(ctx.lr) == 0x822F10E4u) {  // the 500 ms before "Press any button" reads again
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82D35B18(ctx, base);
}


// tw2de (2026-10-25): "Press any button" never opened, so neither its window nor its closing sound (see the top).
// tw2de (2026-11-10): FF XIII's disc swap message sites are gone (one disc); the macro keeps its second list empty.
#define TW2DE_SKIP_AT(fn, disc_sites, ...)                                         \
  extern "C" REX_FUNC(fn) {                                                           \
    const uint32_t lr = uint32_t(ctx.lr);                                             \
    for (const uint32_t site : {__VA_ARGS__})                                         \
      if (On() && lr == site) {                                                       \
        ctx.r3.u64 = 0;                                                               \
        return;                                                                       \
      }                                                                               \
    for (const uint32_t site : std::initializer_list<uint32_t> disc_sites)            \
      if (lr == site) {                                                               \
        ctx.r3.u64 = 0;                                                               \
        return;                                                                       \
      }                                                                               \
    __imp__##fn(ctx, base);                                                           \
  }
#define TW2DE_DISC(...) {__VA_ARGS__}
TW2DE_SKIP_AT(sub_826FAC20, TW2DE_DISC(), 0x822F10C0u)
TW2DE_SKIP_AT(sub_826FA870, TW2DE_DISC(), 0x822F10CCu)
TW2DE_SKIP_AT(sub_826FC110, TW2DE_DISC(), 0x822F1128u)
TW2DE_SKIP_AT(sub_826F9490, TW2DE_DISC(), 0x822F1130u, 0x822F1148u)
#undef TW2DE_DISC
#undef TW2DE_SKIP_AT

// tw2de (2026-10-25, tw2de_diag_boot_sounds): which code plays the sound still heard where "Press any button"
// was. The game's UI sounds go through GuiManager::playSE (sub_827183B8: this, type 0-102) and the layouts' own sound
// events through LayoutProxy::playSe (FF XIII sub_825FD140; not hooked in FF XIII-2 yet). The first 64 of
// each are logged with their callers (the return address and two more from the guest stack's back chain).
REXCVAR_DEFINE_BOOL(tw2de_diag_boot_sounds, false, "Game/Diagnostics",
                    "Log the game's UI sound calls (type or key, and their callers), the first 64 of each kind")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_827183B8);  // GuiManager::playSE(this, type)
REX_EXTERN(__imp__sub_82702670);  // a message: made

namespace {
bool SoundDiagOn() {
  static const bool on = REXCVAR_GET(tw2de_diag_boot_sounds);
  return on;
}
uint32_t Load32(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
// The return addresses above the current call: the caller's caller saved its LR at -8 of the frame its back chain
// points to (the generated prologues' "stw r12,-8(r1)").
void Callers(const uint8_t *base, uint32_t sp, uint32_t out[2]) {
  out[0] = out[1] = 0;
  uint32_t frame = sp;
  for (int i = 0; i < 2; ++i) {
    if (!frame || frame >= 0xF0000000u)
      return;
    frame = Load32(base, frame);
    if (!frame || frame >= 0xF0000000u)
      return;
    out[i] = Load32(base, frame - 8);
  }
}
}  // namespace

// tw2de (2026-10-25): the sound still heard (tw2de_043.log): GuiManager::playSE type 47 from 82574A10 < 824CBA74 <
// 824CC648, i.e. made by the message's own creation (sub_82702670, called for "Press any button" from 0x822011D4; FF XIII-2: 0x822F10B0).
// That creation runs with UI sounds muted on its thread; the message is still made (and later destroyed) as before.
thread_local bool t_mute_ui_sounds = false;

extern "C" REX_FUNC(sub_82702670) {
  if (On() && uint32_t(ctx.lr) == 0x822F10B0u) {
    t_mute_ui_sounds = true;
    __imp__sub_82702670(ctx, base);
    t_mute_ui_sounds = false;
    return;
  }
  __imp__sub_82702670(ctx, base);
}

extern "C" REX_FUNC(sub_827183B8) {
  if (t_mute_ui_sounds)
    return;  // GuiManager::playSE returns nothing
  if (SoundDiagOn()) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 64) {
      uint32_t up[2];
      Callers(base, ctx.r1.u32, up);
      REXLOG_INFO("[sound] GuiManager::playSE type {} from {:08X} < {:08X} < {:08X}", ctx.r4.u32, uint32_t(ctx.lr),
                  up[0], up[1]);
    }
  }
  __imp__sub_827183B8(ctx, base);
}


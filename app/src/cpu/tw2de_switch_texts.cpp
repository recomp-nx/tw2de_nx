// tw2de (2026-10-25, tw2de_switch_device_name): the save menu names the storage device "Nintendo Switch".
//
// tw2de (2026-11-10): FF XIII-2's addresses: the save menu sub_827E76C0, the lookup sub_8272BC28, "$xsave_20" at
// 0x8212B2A8 (the key's address passed only there); the rest of this note is FF XIII's. NOT CHECKED: FF XIII-2's lookup
// keeps its text object otherwise (lhz +8 / lwz +0 where FF XIII's had +12 / +4): the hook takes the text at +0 or +4,
// whichever is the label as the game has it (2026-11-10: back in the build).
// The save screen's device label (top right) is the game's own text, chosen by the device's type in sub_82587420:
// type 1 (hard drive) "$xsave_20", type 2 (memory unit) "$xsave_22" (the key strings at 0x820987D8 / 0x820987E4), looked
// up into a text object by sub_824E0438(text object, key, 0). The SDK's device is the hard drive, so the label is
// $xsave_20: "Xbox 360 Hard Drive" / "Xbox 360 Festplatte" / "Disque dur Xbox 360" / "Disco duro Xbox 360" / "Disco
// rigido Xbox 360" (txtres_us / gr / fr / sp / it.ztr, the line ending "\0\x01"). After that lookup the text the object
// points to (+4) is written over with "Nintendo Switch" and the same ending: every language's original is at least as
// long (19-21 characters against 15), so it is changed in place. The lookup is told apart by the key's address: only
// that site passes it.
//
// tw2de (2026-10-25, tw2de_hide_device_select): no "(Y) Select device" in the save menu, and Y does nothing there.
// The menu's button line is built by sub_82587420 from three texts appended into one buffer by sub_824E8270(buffer,
// key), which puts a separator before every text but the first: "$xsave_16" ("(Y) Select device", key at 0x820987CC,
// return address 0x82588CC0), then "$pause_04" ("(A) OK", only when +268 of the menu is above 0), then
// "$m_res_fl_wmb06" ("(B) Close"). The first append is skipped, so the line starts with OK or Close.
// The Y button: the device change flow, sub_82461588, is reached through the storage manager's vtables (sub_82461AE0:
// r6 1, r7 0; AF0: r6 0, r7 0; B00: r7 1, which returns 0 at once) from the game's job runner (sub_8243C1E8), so the
// save menu does not appear among its callers. tw2de_046.log (tw2de_diag_device_select): one call at start-up (r7 1
// -> 0) and the Y press in the save menu (this 6EC5A8F0, r4 BA540980, r5 0x493B0, r6 1, r7 0 -> -1, from the job
// runner); nothing when the menu opens. Load Game runs the same flow with the same arguments (returning from it at
// once, with 0 or with -1, sent Load Game straight back to the main menu), so the flow runs as it is. What Y adds is
// inside it: with no device set for it ([r24] 0, which Y clears) the flow shows "Select the storage device..."
// ($trc_mes_05 with OK: sub_8245FFE0, return address 0x8246170C), waits 500 ms (sub_8245FB70, 0x82461724), then opens
// the device selector, which the SDK answers at once with the one device. The notice is answered OK without being
// shown (tw2de_skip_boot_prompts.cpp, which owns the sub_8245FFE0 hook) and the wait skipped: Y gives the same device
// back at once and the menu stays.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

REXCVAR_DEFINE_BOOL(tw2de_switch_device_name, true, "Game/Enhancements",
                    "The save menu's storage device label reads \"Nintendo Switch\" instead of \"Xbox 360 Hard Drive\"")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8272BC28);  // the game's text lookup: (text object, key, flags)

namespace {
// tw2de (2026-11-10, LR): the keys found in LR's image ("$xsave_20" 0x8212B2A8, "$xsave_16" 0x8212B29C), each
// passed once: 0x827E8D6C (into the lookup sub_8272BC28) and 0x827E8FA4 (into the append sub_827418B0, return 0x827E8FAC).
constexpr uint32_t kKeyHardDrive = 0x8212B2A8u;  // "$xsave_20" (FF XIII 0x820987D8)
constexpr char kName[] = "Nintendo Switch";
}  // namespace

namespace tw2de {
void NoteTextLookup(const uint8_t *base, uint32_t key, uint32_t lr, uint32_t sp);  // tw2de_menu_trim.cpp
}

extern "C" REX_FUNC(sub_8272BC28) {
  const uint32_t object = ctx.r3.u32, key = ctx.r4.u32;
  tw2de::NoteTextLookup(base, key, uint32_t(ctx.lr), ctx.r1.u32);
  __imp__sub_8272BC28(ctx, base);
  if (key != kKeyHardDrive || !object)
    return;
  static const bool on = REXCVAR_GET(tw2de_switch_device_name);
  if (!on)
    return;
  // tw2de (2026-11-10): FF XIII-2's text object may keep its text at +0 (its lookup's lhz +8 / lwz +0) where FF
  // XIII's had +4: both are tried, and a text is written only when it is the label as the game has it (it holds
  // "Xbox 360" in every language: "Xbox 360 Hard Drive", "Disque dur Xbox 360", ...), so nothing else is touched.
  static constexpr uint32_t kOffsets[] = {0, 4};
  char *p = nullptr;
  size_t length = 0;
  uint32_t at = 0;
  for (const uint32_t off : kOffsets) {
    const uint32_t text = __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + object + off));
    if (!text || text >= 0xF0000000u)
      continue;
    char *q = reinterpret_cast<char *>(base + text);
    const size_t n = strnlen(q, 64);
    if (n == 64 || std::string_view(q, n).find("Xbox 360") == std::string_view::npos)
      continue;
    p = q;
    length = n;
    at = off;
    break;
  }
  if (!p)
    return;
  constexpr size_t kLength = sizeof(kName) - 1;
  if (length < kLength)
    return;  // too short to hold it (no language's is)
  const bool marker = p[length + 1] == '\x01';  // the line's ending after its NUL
  std::memcpy(p, kName, kLength);
  p[kLength] = '\0';
  if (marker)
    p[kLength + 1] = '\x01';
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    REXLOG_INFO("[texts] save menu device label: \"Nintendo Switch\" (the text object's +{}, tw2de_switch_device_name)",
                at);
}

// tw2de (2026-11-10): FF XIII-2's save menu builds its button line as FF XIII's does (sub_827E76C0: three appends by
// sub_827418B0 into one buffer, "$xsave_16" at 0x8212B29C first, return 0x827E8FAC; then "$pause_04" when +0x10C of
// the menu is above 0, then "$m_res_fl_wmb06"). The first append is skipped. Y itself is left as the game has it:
// FF XIII-2's device change flow (sub_8250D2D0, FF XIII's sub_82461588 through the same three vtable stubs at
// 8250D430..450) is built differently and is not hooked; with the SDK's one device, Y gives the same device back.
REXCVAR_DEFINE_BOOL(tw2de_hide_device_select, true, "Game/Enhancements",
                    "The save menu shows no \"(Y) Select device\" (one storage device on the Switch)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_827418B0);  // append a text (by key) to a button line buffer

extern "C" REX_FUNC(sub_827418B0) {
  static const bool hide = REXCVAR_GET(tw2de_hide_device_select);
  if (hide && uint32_t(ctx.lr) == 0x827E8FACu && ctx.r4.u32 == 0x8212B29Cu)
    return;  // "$xsave_16": (Y) Select device
  __imp__sub_827418B0(ctx, base);
}

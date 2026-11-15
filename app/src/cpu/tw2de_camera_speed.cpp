// TW2 (2026-11-16): the camera's turning speed, left-right and up-down, set in the game's own Control options.
//
// 1. What is scaled. The boot menu's "Camera speed" first scaled the right stick itself (2026-11-14, in the SDK's
//    input driver). The panels use that stick too (the map's zoom and pan: ui_nav.swf TryZoomWithStick,
//    RightStickData, fed by Scaleform's own pad axes), so at 50 % the map zoomed at half its speed. Now the stick is
//    the game's, and only what the camera takes from it is scaled.
//    How the game takes it: sub_82E7C470 gives every game input event of the frame to its listeners and then calls
//    sub_82E7A208(game, events) (events: {array, count}, an event is {name id, float value}, 8 bytes), which keeps
//    the axes the game itself uses: GI_AxisLeftX / GI_AxisLeftY at game +0x670 / +0x66C (walking), and the camera's
//    at +0x674 (horizontal) and +0x678 (vertical): GI_AxisRightX / GI_AxisRightY with a pad (the byte at +0x84 of
//    the object at [0x8355B538]), GI_MouseDampX / GI_MouseDampY without one. Those two are read in three places: by
//    sub_82E79AB8 every tick (the active camera's Rotate, its virtual at +0x1F4, with the two values: the same
//    virtual the script native Rotate, sub_824CFCC8, calls) and by the script natives GetCameraHorizontalSpeed /
//    GetCameraVerticalSpeed (sub_82E799C0 / sub_82E799A0). After the game has kept a right stick event's value
//    there, the kept value is the event's times the axis's percent. The events themselves are left as they are.
//
// 2. Where it is set (the user: in Options, Control options, "the same kind of slider as for Gamma", one for each
//    axis, changed while the game runs). cpu/tw2de_switch_ui_core.cpp adds two slider rows to that list in
//    ui_mainmenu.swf (ids 9001 and 9002, the main menu's and the pause menu's: one panel). The panel talks to the
//    game through flash.external.ExternalInterface.call(name, ...), which arrives at sub_82E8A138(interface, &name,
//    arguments, result): name is a string object (its first word the UTF-16 text), arguments {.., count at +4,
//    values at +8}, a value 16 bytes (Scaleform's: the type in the low byte of the word at +4, 3 a number, whose
//    double is at +8), result a holder the game's own answers set with sub_8259DEA0 (a boolean) or sub_8259DF08 (a
//    number). That function knows about twenty names itself and gives the rest to the panel's script by name
//    (sub_82E88490): ItemChanged(id, value) is the list's report of a slider step. Here, before it:
//      - "Tw2CamSpeed"(axis) is answered with the axis's value, 0 to 1 (the rows ask when the list is filled);
//      - ItemChanged of the two ids sets the axis's percent; the game's script never sees it.
//    The slider has 40 places (0 to 39) and shows 0 to 10. At its lowest the camera still turns (5 %).
//
// 3. Where it is kept: camera_speed_x / camera_speed_y in tw2de_settings.ini (the boot menu's file; its own Camera
//    speed row is gone), written about a second after the last step.
#include "cpu/tw2de_switch_ui_core.h"                           // the rows' ids, the value call's name
#include "renut_engine/gpu/nx_shaders/boot_menu_switch.h"      // SaveCameraSpeed

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_INT32(tw2de_camera_percent_x, 100, "Game/Enhancements",
                     "The camera's turning speed left and right in percent of the game's own (the game's Control "
                     "options; kept in tw2de_settings.ini): what the camera takes from the right stick is scaled, "
                     "the stick itself is not")
    .range(0, 100);
REXCVAR_DEFINE_INT32(tw2de_camera_percent_y, 100, "Game/Enhancements",
                     "The camera's turning speed up and down in percent of the game's own (as tw2de_camera_percent_x)")
    .range(0, 100);

REX_EXTERN(__imp__sub_82E7A208);
REX_EXTERN(__imp__sub_82E8A138);  // the panels' ExternalInterface calls: (interface, &name, arguments, result)
REX_EXTERN(__imp__sub_8259DF08);  // a Scaleform value's holder set to a number (holder, f1)

namespace {
constexpr uint32_t kNameAxisRightX = 0x8377BDD8, kNameAxisRightY = 0x8377BDD4;  // where the two names' ids are kept
constexpr uint32_t kCameraHorizontal = 0x674, kCameraVertical = 0x678;          // in the game object
constexpr int kLowestPercent = 5;  // the slider's lowest place: the camera still turns

std::atomic<int> g_percent[2] = {-1, -1};  // left-right, up-down; -1: not read from the settings yet
std::atomic<bool> g_unsaved{false};
std::atomic<int64_t> g_changed_ms{0};

uint32_t Be32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

int Percent(int axis) {
  int v = g_percent[axis].load(std::memory_order_relaxed);
  if (v < 0) {  // the boot menu has set them by the time the game asks
    v = std::clamp(int(axis ? REXCVAR_GET(tw2de_camera_percent_y) : REXCVAR_GET(tw2de_camera_percent_x)), 0, 100);
    g_percent[axis].store(v, std::memory_order_relaxed);
  }
  return v;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The settings file, once the slider has rested (a held direction steps it many times a second).
void SaveWhenSettled() {
  if (!g_unsaved.load(std::memory_order_relaxed) || NowMs() - g_changed_ms.load(std::memory_order_relaxed) < 1000)
    return;
  if (g_unsaved.exchange(false))
    renut::nxs::SaveCameraSpeed(Percent(0), Percent(1));
}

// A string object's UTF-16 text (big-endian) against an ASCII name.
bool NameIs(const uint8_t *wide, const char *name) {
  for (;; wide += 2, ++name) {
    if (wide[0] != 0 || wide[1] != uint8_t(*name))
      return false;
    if (!*name)
      return true;
  }
}

// A Scaleform value that is a number.
bool Number(const uint8_t *value, double &out) {
  if ((Be32(value + 4) & 0x8F) != 3)
    return false;
  uint64_t bits;
  std::memcpy(&bits, value + 8, 8);
  bits = __builtin_bswap64(bits);
  std::memcpy(&out, &bits, 8);
  return true;
}
}  // namespace

extern "C" REX_FUNC(sub_82E7A208) {
  const uint32_t game = ctx.r3.u32, list = ctx.r4.u32;
  __imp__sub_82E7A208(ctx, base);
  SaveWhenSettled();
  const int percent_x = std::max(Percent(0), kLowestPercent), percent_y = std::max(Percent(1), kLowestPercent);
  if ((percent_x >= 100 && percent_y >= 100) || !game || !list)
    return;
  const uint32_t events = Be32(base + list), count = Be32(base + list + 4);
  const uint32_t name_x = Be32(base + kNameAxisRightX), name_y = Be32(base + kNameAxisRightY);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t name = Be32(base + events + i * 8), value = Be32(base + events + i * 8 + 4);
    const uint32_t field = name == name_x ? kCameraHorizontal : name == name_y ? kCameraVertical : 0;
    // Only a value the game has just kept (with a pad): the same bits in the field.
    if (!field || Be32(base + game + field) != value)
      continue;
    float f;
    std::memcpy(&f, &value, 4);
    f *= float(field == kCameraHorizontal ? percent_x : percent_y) * 0.01f;
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    bits = __builtin_bswap32(bits);
    std::memcpy(base + game + field, &bits, 4);
  }
}

extern "C" REX_FUNC(sub_82E8A138) {
  const uint32_t name_at = ctx.r4.u32 ? Be32(base + ctx.r4.u32) : 0, arguments = ctx.r5.u32, result = ctx.r6.u32;
  if (name_at && arguments) {
    const uint8_t *name = base + name_at;
    const uint32_t count = Be32(base + arguments + 4), values = Be32(base + arguments + 8);
    double first = 0.0, second = 0.0;
    if (NameIs(name, tw2de::switch_ui::kCameraValueCall)) {
      const int axis = count >= 1 && values && Number(base + values, first) && first >= 0.5 ? 1 : 0;
      if (result) {
        ctx.r3.u64 = result;
        ctx.f1.f64 = double(Percent(axis)) * 0.01;
        __imp__sub_8259DF08(ctx, base);
      }
      return;
    }
    if (count == 2 && values && NameIs(name, "ItemChanged") && Number(base + values, first) &&
        (int(first) == tw2de::switch_ui::kCameraItemX || int(first) == tw2de::switch_ui::kCameraItemY) &&
        Number(base + values + 16, second)) {
      const int axis = int(first) == tw2de::switch_ui::kCameraItemY ? 1 : 0;
      const int percent = std::clamp(int(std::lround(second * 100.0)), 0, 100);
      if (g_percent[axis].exchange(percent, std::memory_order_relaxed) != percent) {
        g_changed_ms.store(NowMs(), std::memory_order_relaxed);
        g_unsaved.store(true, std::memory_order_relaxed);  // SaveWhenSettled writes and logs it
      }
      return;
    }
  }
  SaveWhenSettled();
  __imp__sub_82E8A138(ctx, base);
}

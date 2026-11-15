// tw2de (2026-10-25, tw2de_switch_buttons): the game's controller button pictures redrawn as the Switch's.
//
// The prompts (battle, menus, the texts' {F1 xx} icons) draw from one sheet, button_set01 (gui/resident/
// system.x360.xgr: 256x128 A8R8G8B8, tiled; the UI layouts' sprites: A, B, X, Y 24x24, LB, RB, LT, RT 38x24, Back and
// Start 22x18 and their names, D-pad and sticks). Its A, B, X, Y become the Switch's round buttons with the same
// letters (the SDK's default mapping: the Switch's A is the game's A; with input_xbox_layout, by position, the letters
// of the Switch's buttons in those places: the game's A is the Switch's B), LB / RB / LT / RT become L / R / ZL / ZR,
// Back and Start become the Switch's - and + buttons (shaped as their marks; their names, drawn under them, are
// cleared), the D-pad sprites the Switch's D-pad (a cross in the game's D-pad grey, pressed arms in its teal fading
// inward); the sticks stay. Drawn in the game's style (glossy); the buttons in the console theme's colours: basic
// white, black buttons with white marks; basic black, white buttons with black marks.
// The sprites (tools/switch_buttons.py: tw2de_switch_buttons.inc, no pixel of the game's) are written over the
// game's sheet in guest memory at Renut's upload of it (found by the XXH3 of its data), as tw2de_images.cpp does.
#include "cpu/tw2de_switch_buttons.h"

#include "cpu/tw2de_lang_pack.h"  // DecodePng
#include "tw2de_romfs.h"  // tw2de (2026-11-06): tex/ in the NRO

#include <rex/cvar.h>
#include <rex/logging.h>

#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "cpu/tw2de_wpd_patch.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(tw2de_switch_buttons, true, "Game/Enhancements",
                    "Button prompts show the Switch's buttons (A B X Y, L R ZL ZR, - +) in the console theme's colours")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, input_xbox_layout);  // sdk/src/input/switch/switch_input_driver.cpp

namespace tw2de {
namespace {

#include "cpu/tw2de_switch_buttons.inc"

constexpr uint64_t kSheetHash = 0x922537DE69485257ull;  // tw2de (2026-11-10): LR's button sheet's data (logged on the console)
constexpr uint32_t kSheetW = 128, kSheetH = 128;
constexpr uint32_t kFmt8888 = 6;                         // GpuTextureFormat::Fmt8_8_8_8

uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t lb) {  // in units (as tw2de_images.cpp)
  const uint32_t aw = (width + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb;
}

// The console's theme: true for basic black.
bool DarkTheme() {
#ifdef __SWITCH__
  bool dark = false;
  if (R_SUCCEEDED(setsysInitialize())) {
    ColorSetId id = ColorSetId_Light;
    if (R_SUCCEEDED(setsysGetColorSetId(&id)))
      dark = id == ColorSetId_Dark;
    setsysExit();
  }
  return dark;
#else
  return false;
#endif
}

std::mutex g_mutex;

}  // namespace

namespace {

// ---- the controller picture (tw2de, 2026-10-25) ----
//
// The camera axis screen after the first battle (layout gr_controller) shows the Xbox pad (texture contorller,
// 256x256 A8R8G8B8, tiled) with a stick moving on it. The pad (0, 0, 256, 181) and the stick's top (neutral
// (2, 222, 34, 34), up / down (37 / 72, 222, 34, 30), left / right (107 / 138, 222, 30, 34)) are drawn over from
// tex/controller.png in the NRO's romfs (tools/switch_controller.py makes it from a photo; tw2de_romfs.h); the
// arrow, the shadow and the shaft are the game's (the shaft darkened, as the Pro Controller's sticks are).
// The layout places the sticks (in the pad's pixels, sprites by their top left corner): nine groups, each with one
// stick still (its shaft and top) and one moving (shadow, shaft, top, arrow). Left-moving groups (at (79, 95): the
// moving stick at -44 / -39, so the Xbox pad's left cap centre at (52, 73)) and right-moving groups (at (145, 95):
// at 0 / 1, the right cap centre at (163, 113)); their still sticks at (66, 0) / (67, 1) and (-111, -40) /
// (-110, -39). controller.ini gives the new pad's cap centres; the groups are moved so the moving stick is on its
// cap, the still ones so they are on the other.
constexpr uint64_t kPadHash = 0xDE2F841E0604465Cull;     // contorller's data
constexpr uint64_t kLayoutHash = 0x78DA37BF32858A7Bull;  // gr_controller
constexpr uint16_t kPadParts[][4] = {{0, 0, 256, 181}, {2, 222, 34, 34}, {37, 222, 34, 30}, {72, 222, 34, 30},
                                     {107, 222, 30, 34}, {138, 222, 30, 34}};
constexpr uint32_t kLeftGroups[] = {0x280, 0x13C0, 0x2400, 0x3540};          // at (79, 95)
constexpr uint32_t kLeftStill[] = {0x2F0, 0x1430, 0x29C0, 0x3B00};           // their right shaft (66, 0); top +0x80
constexpr uint32_t kRightGroups[] = {0xB20, 0x1BE0, 0x2CA0, 0x3DE0, 0x4680};  // at (145, 95)
constexpr uint32_t kRightStill[] = {0xB90, 0x1C50, 0x3260, 0x43A0, 0x46F0};  // their left shaft (-111, -40); top +0x80

std::vector<uint8_t> g_pad;  // controller.png, RGBA 256x256
float g_left[2] = {0, 0}, g_right[2] = {0, 0};

void LoadPad() {
  // tw2de (2026-11-06): from the NRO's romfs (the repository's tex folder), not the SD card.
  // tw2de (2026-11-09): read into RAM at the start (tw2de_romfs.h).
  const std::vector<uint8_t> *ini = tw2de::romfs::Find("tex/controller.ini");
  const std::vector<uint8_t> *png_file = tw2de::romfs::Find("tex/controller.png");
  if (!ini)
    return;
  int got = 0;
  {
    const std::string text(ini->begin(), ini->end());
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find('\n', start);
      if (end == std::string::npos)
        end = text.size();
      const std::string line = text.substr(start, end - start);
      start = end + 1;
      if (std::sscanf(line.c_str(), " left = %f %f", &g_left[0], &g_left[1]) == 2)
        got |= 1;
      else if (std::sscanf(line.c_str(), " right = %f %f", &g_right[0], &g_right[1]) == 2)
        got |= 2;
    }
  }
  if (!png_file || got != 3) {
    REXLOG_WARN("[buttons] controller.ini without left and right, or no controller.png: the controller picture stays");
    return;
  }
  const std::vector<uint8_t> &png = *png_file;
  int w = 0, h = 0;
  if (!DecodePng(png, w, h, g_pad) || w != 256 || h != 256) {
    g_pad.clear();
    REXLOG_ERROR("[buttons] controller.png is not a 256x256 PNG: the controller picture stays");
    return;
  }
  // The layout, as its file is loaded.
  WpdPatchRegister("gr_controller", "ykd", [](uint8_t *data, uint32_t size) {
    auto get = [&](uint32_t o, float v[2]) { std::memcpy(v, data + o + 0x30, 8); };
    auto put = [&](uint32_t o, float x, float y) {
      const float v[2] = {x, y};
      std::memcpy(data + o + 0x30, v, 8);
    };
    if (size < 0x4900 || XXH3_64bits(data, size) != kLayoutHash) {
      REXLOG_WARN("[buttons] gr_controller: not the release's layout: the sticks stay where they were");
      return;
    }
    float v[2];
    get(kLeftGroups[0], v);
    if (v[0] != 79 || v[1] != 95)
      return;
    const float lx = g_left[0], ly = g_left[1], rx = g_right[0], ry = g_right[1];
    // Left-moving: the moving top (34x34 at -44, -39) centred on the left cap; the still right shaft (36x36) on
    // the right cap, its top one pixel right and down (as the game's).
    const float ax = lx + 27, ay = ly + 22;
    for (size_t i = 0; i < 4; ++i) {
      put(kLeftGroups[i], ax, ay);
      put(kLeftStill[i], rx - 18 - ax, ry - 18 - ay);
      put(kLeftStill[i] + 0x80, rx - 17 - ax, ry - 17 - ay);
    }
    const float bx = rx - 18, by = ry - 18;
    for (size_t i = 0; i < 5; ++i) {
      put(kRightGroups[i], bx, by);
      put(kRightStill[i], lx - 18 - bx, ly - 18 - by);
      put(kRightStill[i] + 0x80, lx - 17 - bx, ly - 17 - by);
    }
    REXLOG_INFO("[buttons] gr_controller: sticks moved to the new pad's ({:.1f}, {:.1f}) and ({:.1f}, {:.1f})", lx, ly,
                rx, ry);
  });
}

void PatchPad(uint8_t *data) {
  for (const auto &part : kPadParts)
    for (uint32_t y = part[1]; y < uint32_t(part[1] + part[3]); ++y)
      for (uint32_t x = part[0]; x < uint32_t(part[0] + part[2]); ++x) {
        const uint8_t *p = &g_pad[(size_t(y) * 256 + x) * 4];
        uint8_t *o = data + size_t(TiledOffset(x, y, 256, 2)) * 4;
        o[0] = p[3];
        o[1] = p[0];
        o[2] = p[1];
        o[3] = p[2];
      }
  // The game's shaft (63, 182, 36, 36: a light grey dome, the Xbox pad's) darkened to the Pro Controller's sticks.
  for (uint32_t y = 182; y < 218; ++y)
    for (uint32_t x = 63; x < 99; ++x) {
      uint8_t *o = data + size_t(TiledOffset(x, y, 256, 2)) * 4;
      for (int k = 1; k < 4; ++k)
        o[k] = uint8_t(o[k] * 2 / 5);
    }
  REXLOG_INFO("[buttons] the controller picture: controller.png");
}

}  // namespace

void SwitchButtonsInit() {
  if (SwitchButtonsActive())
    LoadPad();
}

bool SwitchButtonsActive() {
  static const bool on = REXCVAR_GET(tw2de_switch_buttons);
  return on;
}

void SwitchButtonsPatch(uint8_t *data, uint32_t format, uint32_t width, uint32_t height, bool tiled) {
  if (!SwitchButtonsActive() || format != kFmt8888 || !tiled)
    return;
  if (width == 256 && height == 256) {
    if (!g_pad.empty() && XXH3_64bits(data, 256 * 256 * 4) == kPadHash)
      PatchPad(data);
    return;
  }
  // tw2de (2026-11-10): LR's button sheet is 128x128 (tools/switch_buttons.py SHEET / PARTS).
  if (width != kSheetW || height != kSheetH)
    return;
  const uint64_t hash = XXH3_64bits(data, size_t(kSheetW) * kSheetH * 4);
  if (hash != kSheetHash) {
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 16)
      REXLOG_INFO("[buttons] a {}x{} A8R8G8B8 texture, not the button sheet: hash {:016X}", width, height, hash);
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  static std::vector<uint8_t> rgba;  // the sprites, decoded once
  static bool tried = false;
  static bool dark = false, position = false;
  if (!tried) {
    tried = true;
    dark = DarkTheme();
    position = REXCVAR_GET(input_xbox_layout);
    const uint8_t *png = dark ? (position ? kButtons_dark_position : kButtons_dark_letter)
                              : (position ? kButtons_light_position : kButtons_light_letter);
    const size_t size = dark ? (position ? sizeof(kButtons_dark_position) : sizeof(kButtons_dark_letter))
                             : (position ? sizeof(kButtons_light_position) : sizeof(kButtons_light_letter));
    int w = 0, h = 0;
    if (!DecodePng(std::vector<uint8_t>(png, png + size), w, h, rgba) || w != int(kSheetW) || h != int(kSheetH)) {
      rgba.clear();
      REXLOG_ERROR("[buttons] the built-in button pictures could not be read");
    }
  }
  if (rgba.empty())
    return;
  for (const auto &part : kButtonParts)
    for (uint32_t y = part[1]; y < uint32_t(part[1] + part[3]); ++y)
      for (uint32_t x = part[0]; x < uint32_t(part[0] + part[2]); ++x) {
        const uint8_t *p = &rgba[(size_t(y) * kSheetW + x) * 4];
        uint8_t *o = data + size_t(TiledOffset(x, y, kSheetW, 2)) * 4;  // ARGB, big-endian
        o[0] = p[3];
        o[1] = p[0];
        o[2] = p[1];
        o[3] = p[2];
      }
  REXLOG_INFO("[buttons] button prompts: the Switch's ({} theme, {})", dark ? "basic black" : "basic white",
              position ? "by position (input_xbox_layout)" : "by letter");
}

}  // namespace tw2de

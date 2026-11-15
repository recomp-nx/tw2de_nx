// TW2 (2026-11-14): TW2's interface on the Switch (tw2de_switch_ui).
//
// 1. The information screen after the title no longer advises installing the game to the hard drive. The game's script
//    asks its native ShowInstallInfo (sub_82E61580: true when the game's media type, the word at 0x8355A9A4, is 1, the
//    disc) and then shows the panel's text; here it answers false. The media type itself is left alone (the game reads
//    it in five other places).
// 2. A patched view of the game's archive, game:\data\pack0.dzip (the file on the card is never written: the SDK lays
//    ranges of bytes over what the game reads of it, rex::filesystem::FilePatch in host_path_file.h; the view is built
//    once, at the archive's first open, by tw2de_switch_ui_core.cpp, in about the time of reading its 2.8 MB table):
//    - the controller configuration screen (Options, Control options) shows a Pro Controller instead of the Xbox 360's
//      pad: tex/tw2_pad.png over the screen's own background, in globals\gui\ccontrols.swf's picture;
//    - the button pictures are the Switch's: A B X Y black and white, L / R for LB / RB, ZL / ZR for LT / RT, minus and
//      plus for Back and Start, the Pro Controller's D-pad with the pressed arm lit (tex/tw2_pad_icons.png, drawn by
//      tools/tw2_switch_ui_art.py). The game has them in two places: inside its .swf files (cnormalbutton.swf's 14, the
//      prompts of every screen, and the copies ten other files carry: found by their data's hash) and as files
//      (globals\gui\icons\buttons: 322 in three forms, the same 14 in each language's folder, for the texts' icons). The letters are the ones to press: the SDK maps the face buttons by letter (the
//      Switch's A is the game's A); with input_xbox_layout (by position) A / B and X / Y are exchanged, as the pad is
//      at the game's start;
//    - the options menu has no "select storage device" row (the Xbox 360's device selector): globals\gui\
//      ui_mainmenu.swf's list leaves the row with that id out.
// tools/tests/tw2_switch_ui_test.cpp and tw2_switch_ui_check.py build and check the same view on a PC.
#include "cpu/tw2de_switch_ui.h"

#include "cpu/tw2de_lang_pack.h"  // DecodePng
#include "cpu/tw2de_switch_ui_core.h"
#include "renut_engine/gpu/nx_shaders/boot_menu_switch.h"  // CameraSpeedLabel
#include "tw2de_romfs.h"

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_file.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <sys/types.h>  // off_t
#endif

REXCVAR_DEFINE_BOOL(tw2de_switch_ui, true, "Game/Enhancements",
                    "The game's interface for the Switch: a Pro Controller on the controller configuration screen, "
                    "the Switch's button pictures (A B X Y black and white, L R ZL ZR, - +, its D-pad), no \"select "
                    "storage device\" in the options, no advice to install the game to the hard drive")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, input_xbox_layout);  // sdk/src/input/switch/switch_input_driver.cpp
REXCVAR_DECLARE(std::string, tw2de_game_language);  // cpu/tw2de_language.cpp: the game's language code

REX_EXTERN(__imp__sub_82E61580);  // the script native ShowInstallInfo(object, stack r4, result r5)

// The native's own two steps, with the answer false: the script's code pointer (stack +0x18) past the call's end
// marker, then the result (a byte), when the script wants one.
extern "C" REX_FUNC(sub_82E61580) {
  if (!REXCVAR_GET(tw2de_switch_ui)) {
    __imp__sub_82E61580(ctx, base);
    return;
  }
  uint8_t *code = base + ctx.r4.u32 + 0x18;
  const uint32_t next = (uint32_t(code[0]) << 24 | uint32_t(code[1]) << 16 | uint32_t(code[2]) << 8 | code[3]) + 1;
  code[0] = uint8_t(next >> 24);
  code[1] = uint8_t(next >> 16);
  code[2] = uint8_t(next >> 8);
  code[3] = uint8_t(next);
  if (const uint32_t result = ctx.r5.u32)
    base[result] = 0;
}

namespace tw2de {
namespace {

bool LoadPng(const char *name, switch_ui::Image &out) {
  const std::vector<uint8_t> *file = romfs::Find(name);
  if (!file || !DecodePng(*file, out.w, out.h, out.rgba)) {
    REXLOG_WARN("[switch ui] {} is not in the NRO's romfs (or not a PNG)", name);
    return false;
  }
  return true;
}

bool IsArchive(std::string_view path) {
  static constexpr std::string_view kName = "pack0.dzip";
  if (path.size() < kName.size())
    return false;
  const std::string_view tail = path.substr(path.size() - kName.size());
  for (size_t i = 0; i < kName.size(); i++) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z')
      c = char(c + 32);
    if (c != kName[i])
      return false;
  }
  if (path.size() == kName.size())
    return true;
  const char before = path[path.size() - kName.size() - 1];
  return before == '\\' || before == '/' || before == ':';
}

bool Seek(std::FILE *f, uint64_t offset) {
#if defined(_WIN32)
  return _fseeki64(f, int64_t(offset), SEEK_SET) == 0;
#else
  return fseeko(f, off_t(offset), SEEK_SET) == 0;
#endif
}

std::shared_ptr<const rex::filesystem::FilePatch> Provide(std::string_view guest_path,
                                                          const std::filesystem::path &host_path, uint64_t size) {
  if (!IsArchive(guest_path))
    return nullptr;
  const auto start = std::chrono::steady_clock::now();
  switch_ui::Assets assets;
  LoadPng("tex/tw2_pad_icons.png", assets.icons);
  LoadPng("tex/tw2_pad.png", assets.pad);
  assets.xbox_layout = REXCVAR_GET(input_xbox_layout);
  // TW2 (2026-11-16): the control options' camera speed rows (cpu/tw2de_camera_speed.cpp), named in the language
  // the game starts in.
  assets.camera_rows = true;
  assets.camera_label_x = renut::nxs::CameraSpeedLabel(REXCVAR_GET(tw2de_game_language), 0);
  assets.camera_label_y = renut::nxs::CameraSpeedLabel(REXCVAR_GET(tw2de_game_language), 1);
  std::FILE *f = std::fopen(host_path.string().c_str(), "rb");
  if (!f) {
    REXLOG_WARN("[switch ui] {} could not be opened: the game's interface stays as it is", host_path.string());
    return nullptr;
  }
  switch_ui::Result result;
  const bool ok = switch_ui::BuildPack0Patch(
      [f](uint64_t offset, void *out, size_t count) { return Seek(f, offset) && std::fread(out, 1, count, f) == count; },
      size, assets, result);
  std::fclose(f);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (!result.notes.empty())
    REXLOG_WARN("[switch ui] left as it is: {}", result.notes);
  if (!ok)
    return nullptr;
  size_t held = 0;
  for (const switch_ui::Range &range : result.ranges)
    held += range.data.size();
  REXLOG_INFO("[switch ui] {}: {} button picture files and {} pictures in {} .swf files ({}), the controller picture "
              "{}, the options menu's storage row {}, the camera speed rows {}; {} ranges, {:.0f} KB past the archive's "
              "end ({:.0f} KB of them in memory), {:.0f} ms",
              std::string(guest_path), result.icon_files, result.swf_pictures, result.swf_files,
              assets.xbox_layout ? "by position" : "by letter", result.pad ? "replaced" : "kept",
              result.menu ? "removed" : "kept", result.camera ? "added" : "not added", result.ranges.size(),
              double(result.size - size) / 1024.0,
              double(held) / 1024.0, ms);
  auto patch = std::make_shared<rex::filesystem::FilePatch>();
  patch->real_size = size;
  patch->size = result.size;
  patch->ranges.reserve(result.ranges.size());
  for (switch_ui::Range &range : result.ranges) {
    rex::filesystem::FilePatch::Range r;
    r.offset = range.offset;
    r.data = std::move(range.data);
    r.source = range.source;
    r.source_size = range.source_size;
    patch->ranges.push_back(std::move(r));
  }
  return patch;
}

}  // namespace

void SwitchUiInit() {
  if (REXCVAR_GET(tw2de_switch_ui))
    rex::filesystem::SetFilePatchProvider(&Provide);
}

}  // namespace tw2de

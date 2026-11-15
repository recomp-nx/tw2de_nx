// TW2 (2026-11-14): the patched view of TW2's archive (pack0.dzip) that tw2de_switch_ui.cpp serves: built
// here from the archive itself and the port's own pictures, with nothing of the runtime's (so tools/tests can build
// it on a PC: tools/tests/tw2_switch_ui_test.cpp). See tw2de_switch_ui.cpp for what it changes and why.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tw2de::switch_ui {

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // w * h * 4, rows top down, not premultiplied
};

struct Assets {
  Image icons;  // tex/tw2_pad_icons.png (tools/tw2_switch_ui_art.py): 16 cells of 32x32 a row, 2 rows
  Image pad;    // tex/tw2_pad.png: 512x358
  bool xbox_layout = false;  // input_xbox_layout: the face buttons by position (the game's A is the Switch's B)
  bool storage_item = false;  // keep the options menu's "select storage device"
  // TW2 (2026-11-16): two slider rows after the control options' own (the camera's speed, left-right and up-down),
  // with these names (UTF-8, in the game's language). cpu/tw2de_camera_speed.cpp answers them.
  bool camera_rows = false;
  std::string camera_label_x, camera_label_y;
};

// The two rows' ids (the game's own are below 100) and what ui_mainmenu.swf asks the game for their values with.
constexpr int kCameraItemX = 9001, kCameraItemY = 9002;
constexpr char kCameraValueCall[] = "Tw2CamSpeed";

// `data` at `offset` of the patched view; or, with no data, `source_size` bytes of the archive itself from `source`.
struct Range {
  uint64_t offset = 0;
  std::vector<uint8_t> data;
  uint64_t source = 0, source_size = 0;
};

struct Result {
  uint64_t size = 0;          // the patched file's size
  std::vector<Range> ranges;  // sorted by offset, not overlapping; the last ones lie past the file's own end
  int icon_files = 0;         // button picture files replaced (archive entries)
  int swf_pictures = 0;       // button pictures replaced inside .swf files
  int swf_files = 0;          // .swf files changed
  bool pad = false;           // the controller picture
  bool menu = false;          // the options menu without its storage device item
  bool camera = false;        // the control options with the two camera speed rows
  std::string notes;          // what was left as it is, and why
};

// Reads `size` bytes of the archive at `offset`; false when it cannot.
using ReadFn = std::function<bool(uint64_t offset, void *out, size_t size)>;

// False (and `out.notes`) when the archive is not what is expected; a part that cannot be made is left out and noted.
bool BuildPack0Patch(const ReadFn &read, uint64_t file_size, const Assets &assets, Result &out);

}  // namespace tw2de::switch_ui

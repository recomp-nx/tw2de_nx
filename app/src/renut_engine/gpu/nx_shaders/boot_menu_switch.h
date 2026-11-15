// tw2de (2026-10-25): the boot menu on the Switch (boot_menu_switch.cpp): language, frame interval, resolution,
// anti-aliasing and upscaler, kept in tw2de_settings.ini next to the NRO.
#pragma once

#include <filesystem>
#include <string>

#include <switch.h>

namespace renut::nxs {

// Before the shader pre-pass and the renderer's swapchain (FnxfXiiiLrApp::OnPreSetup). Shows the menu when the
// settings file does not exist (the first start) or A is held at start (0.5 s), then applies the
// settings (the options they set, over tw2de.toml's). Closing the software from HOME in the menu exits.
// user_data_root: the runtime's (tw2de 2026-10-28: the achievement lists read its achievements/ folder).
void RunBootMenu(const std::filesystem::path &user_data_root);

// TW2 (2026-11-16): the camera's speed is set in the game's own Control options (cpu/tw2de_camera_speed.cpp) and
// kept in this menu's file: its two values written there (0-100, left-right and up-down).
void SaveCameraSpeed(int x_percent, int y_percent);
// The two rows' names in the game's language (its code: "PL", "EN", ...; another: English), axis 0 X, 1 Y.
std::string CameraSpeedLabel(const std::string &game_code, int axis);

// tw2de (2026-11-07): whether the pre-pass and boot menu screens should be 1920x1080: docked (Reverse-NX's mode when
// it rules) on a 1080p display resolution, as the game's output (render/graphics_options.cpp; tw2de_diag_output 1:
// never, 2: always). Asks the applet at most twice a second.
bool ScreenWants1080p();

// tw2de (2026-11-07): libnx's framebuffer on the default window at 1280x720 or 1920x1080 (the window's size set to
// it while it is open, 1280x720 again after: the renderer's swapchain sets its own).
struct ScreenFramebuffer {
  Framebuffer fb{};
  bool open = false;
  bool hd = false;
  bool Open(bool want_1080p);
  void Close();
};

// TW2 (2026-11-15): framebufferEnd only while the window has a buffer dequeued. libnx's copies the picture to
// "the window's current slot" without looking; with none (slot -1) that is 3.75 MB before the buffers: the crash of
// 2026-11-15 (see ScreenFramebuffer::Close).
inline void ScreenFramebufferEnd(Framebuffer &fb) {
  if (fb.win && fb.win->cur_slot >= 0)
    framebufferEnd(&fb);
}

}  // namespace renut::nxs

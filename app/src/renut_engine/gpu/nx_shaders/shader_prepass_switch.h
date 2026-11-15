// tw2de (2026-10-21): the shader pre-pass's screen and threads on the Switch (shader_prepass_switch.cpp).
#pragma once

#include <string>

namespace renut::nxs {

// Before the game starts and before the renderer makes its swapchain: returns at once when every game shader is
// already in the store and compiled (or tw2de_shader_prepass is off).
void RunSwitchShaderPrepass(const std::string &game_root);

// tw2de (2026-10-21): started as an applet (from the album: about 400 MB, the game needs far more) rather than as
// an application (title takeover).
bool RunningAsApplet();
// Applet mode is handled before main (shader_prepass_switch.cpp, AppletModeGuard): the pre-pass's screen says it is not
// supported and to start the game with R held (title takeover), and the process returns to hbloader.

}  // namespace renut::nxs

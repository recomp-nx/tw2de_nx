// tw2de (2026-10-25, tw2de_switch_buttons): the game's button pictures as the Switch's (tw2de_switch_buttons.cpp).
#pragma once

#include <cstdint>

namespace tw2de {

bool SwitchButtonsActive();
// Once the config is loaded (FnxfXiiiLrApp::OnPreSetup), before the game's files are read: the controller picture
// (tex/controller.png and .ini in the NRO's romfs, when there).
void SwitchButtonsInit();
// render/textures.cpp Upload, before a one-level texture's data is read (Renut's format, GpuTextureFormat).
void SwitchButtonsPatch(uint8_t *data, uint32_t format, uint32_t width, uint32_t height, bool tiled);

}  // namespace tw2de

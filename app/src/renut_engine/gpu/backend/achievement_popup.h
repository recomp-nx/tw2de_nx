// tw2de (2026-10-23): "Achievement unlocked" at the top left of the picture when the game unlocks one.
//
// The approach of xc2fsr's menu: the CPU draws a small RGBA image (a box, the header in the game's language and the
// achievement's name, from a bitmap font with the letters English, German, French, Spanish and Italian need:
// popup_compose.h, popup_font.h) and it is put over the top left corner of the frame right before the present. Here the
// GPU blends it on in the present pass (shaders/host/popup_ps.hlsl), as the compile notice is drawn.
//
// The SDK's achievement manager calls the notification callback (the guest thread that unlocked it): the popup is
// queued, and the render thread makes its image and shows it, fading in and out, one at a time.
#pragma once

#include <cstdint>

namespace plume {
class RenderCommandList;
}

namespace renut::gpu {
struct VideoState;

namespace achievement_popup {

// After the present pipeline is built; caller holds s.mutex.
void InitLocked(VideoState &s);
// Before the device goes; caller holds s.mutex and has waited for the GPU.
void ShutdownLocked(VideoState &s);
// In PresentLocked with the back buffer bound: the popup, if one is up. (x, y): the top left corner of the picture on
// the output (the game image may be letterboxed); top: room to leave above it (the compile notice's).
bool DrawLocked(VideoState &s, plume::RenderCommandList *cmd, uint32_t out_w, uint32_t out_h, int32_t x, int32_t y,
                int32_t top);

}  // namespace achievement_popup
}  // namespace renut::gpu

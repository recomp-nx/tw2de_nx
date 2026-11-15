// tw2de (2026-10-22): "Compiling shaders..." at the top left of the screen while the deko3d back end compiles a
// shader the game waits for, and for a moment after. Drawn by the GPU (shaders/host/notice_ps.hlsl) from a texture of
// the text in the Switch's shared font, rasterised once at start-up; no per-frame CPU work besides one draw.
//
// A compile holds the render thread, so no frame is presented while it runs: at its start the notice is drawn
// straight onto the swapchain image last presented (the one on screen, or queued for it) with a list of its own. The
// presents after a compile carry it too, for kHoldSeconds.
#pragma once

#include <cstdint>

namespace plume {
class RenderCommandList;
}

namespace renut::gpu {
struct VideoState;

namespace compile_notice {

// After the present pipeline is built; caller holds s.mutex. Failure leaves the notice off.
void InitLocked(VideoState &s);
// Before the device goes; caller holds s.mutex and has waited for the GPU.
void ShutdownLocked(VideoState &s);
// In PresentLocked with the back buffer bound: the notice, when a compile ran recently.
bool DrawLocked(VideoState &s, plume::RenderCommandList *cmd, uint32_t out_w, uint32_t out_h);
// After that frame's present: which swapchain image it was, and whether the notice is on it.
void NotePresentedLocked(VideoState &s, uint32_t image_index, bool notice_drawn);
// Whether DrawLocked would draw now.
bool Active();
// tw2de (2026-10-23): the bottom of the notice on the output when it is drawn (0 when not): the achievement popup
// goes below it.
int32_t BottomLocked();

}  // namespace compile_notice
}  // namespace renut::gpu

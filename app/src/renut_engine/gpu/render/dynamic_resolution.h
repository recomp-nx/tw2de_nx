/**
 * @file    gpu/render/dynamic_resolution.h
 * @brief   tw2de (2026-11-07): dynamic resolution of the scene.
 *
 * The scene's targets (graphics_options.cpp SurfaceRenderScale: RenderScale::drs) keep their host size, the one
 * tw2de_render_height gives; each frame only g_drs_factor of it is drawn, from the top-left corner. The draws'
 * viewports and scissors, the clears and the resolves are scaled by it (RenderScale::Coord, DrawFactor); what is
 * resolved from such a target keeps the flag (HostTexture::scale), and the shaders that sample it scale their texture
 * coordinates by the factor and clamp them to the drawn part (shader_common_glsl.h renutDrsUv: the descriptor index's
 * kDrsTextureBit, SharedConstants::drs_fill). FSR 3.1 gets the drawn size as its render size (its textures at the
 * full one). The factor follows the GPU's frame time (as xc1fsr's dynamic resolution: its levels, margin, hold and
 * memory of the loads seen), so the frame interval's frame time is kept with a margin.
 */
#pragma once

#include <cstdint>

namespace renut::gpu {

// A texture resolved from a dynamic-resolution target carries the level it was drawn at in its descriptor index, bits
// 20-25 (free: the slot is bits 0-19, the scale code 26-28, the stacked flags 29-31): 0 none, n the drawn fraction
// 1 - (n - 1) x the step (SharedConstants::drs_step; shader_common_glsl.h renutDrsUv). Per texture, so a texture
// resolved in an earlier frame at another level is read as it was drawn.
constexpr uint32_t kDrsCodeShift = 20;
constexpr uint32_t kDrsCodeMax = 63;

// Dynamic resolution is on (tw2de_drs or tw2de_diag_drs_fixed; deko3d; read once).
bool DrsOn();
// The frame's factor: chosen at the frame's start (before FSR 3.1's jitter and the frame's first draw).
void DrsFrameStartLocked();
// The GPU time of a finished frame (backend/video.cpp CollectGpuTime), in ms.
void DrsOnGpuFrame(double ms);
// tw2de (2026-11-08): a draw of the 3D scene (geometry with a depth target into a dynamic-resolution colour target).
// Dynamic resolution lowers the resolution only in frames that follow one with at least kDrsSceneDraws of them (3D
// gameplay and real-time scenes); menus, the title, loading screens and prerendered movies stay at full resolution, and
// their GPU time does not steer it.
// tw2de (2026-11-08): 'geometry': the transform on, depth tested and written (only those make a frame 3D gameplay;
// the others are counted for the log).
void DrsNoteSceneDraw(bool geometry);
// A host size as drawn this frame (the factor of the dynamic-resolution targets).
uint32_t DrsDrawn(uint32_t host);
// This frame's level as a texture's code (1 + the level; 0 without dynamic resolution), the step between two levels'
// fractions, and a code's fraction.
uint32_t DrsCode();
float DrsStep();
float DrsFillOfCode(uint32_t code);
// TW2 (2026-11-14, tw2de_drs_sized): the levels as target sizes (render_scale.h kDrsFirst). The frame's scale of the
// scene's targets; g_drs_factor stays 1 and no texture carries a code.
bool DrsSized();
struct RenderScale;
RenderScale DrsSceneScale();

}  // namespace renut::gpu

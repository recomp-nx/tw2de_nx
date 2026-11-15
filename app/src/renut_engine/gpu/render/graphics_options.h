/**
 * @file    gpu/render/graphics_options.h
 * @brief   Graphics quality beyond the console's (Nuts&Bolts/Graphics):
 *          shadow map and ambient occlusion resolution, anisotropic
 *          filtering.
 *
 * - renut_shadow_resolution: the shadow maps' render scale. The sun's two
 *   cascades are 1024x1024 and 768x768 D24S8 targets (512x512 in split
 *   screen; sub_822998C8), and the local lights render 512x512 D24S8 depth
 *   converted in a 512x512 16_16_FLOAT target into a 2048x2048 atlas
 *   (sub_823B3CE0); by default they follow the render resolution.
 * - renut_ambient_occlusion: the SSAO pass (sub_8235FA90) renders at half
 *   the screen, into 640x360 targets at EDRAM base 1864; Full renders it at
 *   the screen's resolution.
 * - renut_anisotropic_filtering: the least a texture's linear minification
 *   filters anisotropically.
 * - renut_post_process_precision: the 8_8_8_8 targets of half the screen or
 *   less (bloom, depth of field, SSAO) as 16-bit unorm on the host. The bloom
 *   (bright pass at 640x360 from the 8-bit scene, then 320x180 down to 80x45,
 *   added at weights 1, 1.2, 1.6 and 2) carries a faint glow in 1/255 steps;
 *   upscaled to the screen they show as blobs that slide over dark areas as
 *   the camera moves. A resolve's destination takes its source's host format,
 *   so the passes reading them see the finer values too.
 */
#pragma once

#include <cstdint>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/render_scale.h"

namespace renut::gpu {

// Reads the options; the rest use what it read. Once a frame, with the render
// resolution (LatchResolutionScaleLocked), so a frame never mixes them.
void LatchGraphicsOptionsLocked();

// The render scale of the target 'desc' describes, given the render
// resolution's.
RenderScale SurfaceRenderScale(const d3d::SurfaceDesc &desc, RenderScale resolution);
// tw2de (2026-11-08): a target of the scene's size, or a column or row of it while another scene than 1280x720
// tiles (graphics_options.cpp).
bool OutputSceneSized(uint32_t width, uint32_t height);

// tw2de (2026-11-07, tw2de_test_output_1080p): a 1920x1080 output (the 1280x720 targets at 1.5x, the window
// 1920x1080). The latch returns true when it changed (the targets are then made again: a new surface epoch).
bool LatchOutput1080pLocked();
// tw2de (2026-11-07): a 1280x720 scene's own single-sampled 1280x720 targets (its post-processing) stay 1x; those
// drawn after its picture's copy into the scene's colour target (the copy's target, the HUD's) are promoted to 1.5x
// for a 1920x1080 output (draw.cpp). Returns true when newly promoted (the targets are made again: a new epoch).
uint64_t OutputTargetKey(const d3d::SurfaceDesc &desc);
bool PromoteOutputTargetLocked(uint64_t out_key);
// tw2de (2026-11-07): the frame's part after that copy (true from its draw to the frame's end): a promoted target is
// 1.5x then, 1x before it (its post-processing, through the same EDRAM view). Returns true when it changed (a new
// epoch: the surfaces looked up again).
bool SetOutputPhaseLocked(bool after_copy);
bool Output1080p();

// The host format of the color target 'desc' describes.
plume::RenderFormat SurfaceColorFormat(const d3d::SurfaceDesc &desc);

// The anisotropy linear minification gets at least (1 = the game's own).
uint32_t MinAnisotropy();

} // namespace renut::gpu

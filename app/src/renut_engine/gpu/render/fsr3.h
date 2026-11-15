// tw2de (2026-11-06, tw2de_fsr3): AMD FidelityFX Super Resolution 3.1's upscaler (render/fsr3.cpp) in place of
// the game's upscale draw, with the scene's per-object motion vectors (render/draw.cpp, tw2de_motion_vectors).
#pragma once

#include <cstdint>

#include <plume_render_interface.h>  // tw2de (2026-11-08): RenderBox, RenderTexture

namespace plume {
struct RenderTexture;
}

namespace renut::gpu {

struct HostSurface;

// What the scene left for FSR this frame (render/draw.cpp MotionSceneInputsLocked).
struct Fsr3SceneInputs {
  plume::RenderTexture *motion = nullptr;  // RG16F: previous minus current position in normalized device coordinates
  HostSurface *depth = nullptr;            // the scene's depth target (0 near, 1 far)
  uint32_t width = 0, height = 0;          // the render size (host pixels): as drawn this frame
  // tw2de (2026-11-07, dynamic resolution): the inputs' host size (the largest render size; FSR's textures).
  uint32_t max_width = 0, max_height = 0;
  bool single_sample = false;              // FSR reads single-sampled targets only
  // The scene's projection (row-vector, as the game's c165-c168): x and y scale, and depth's A and B (z' = A z + B).
  float proj_x = 0, proj_y = 0, proj_a = 0, proj_b = 0;
  bool cut = false;  // most scene draws had no match in the previous frame: the history is reset
  // TW2 (2026-11-15): the scene's near and far planes when known (both > 0: used instead of proj_a / proj_b), and how
  // the depth target's value becomes 0 (near) .. 1 (far): value * depth_scale + depth_bias, then clamped. TW2
  // draws its scene with a reversed depth range (near 1, far 0.1: viewport z scale -0.9, offset 1).
  float near_z = 0, far_z = 0;
  float depth_scale = 1.0f, depth_bias = 0.0f;
  // tw2de (2026-11-07, tw2de_diag_fsr3_frames): the frame (render/draw.cpp's frame start count), this frame's
  // scene draws with / without last frame's constants, and the jitter the jittered scene draws used (NDC; the first
  // one's, and whether a later one differed).
  uint64_t frame = 0;
  uint64_t matched = 0, unmatched = 0, jittered = 0;
  float drawn_jitter[2] = {};
  bool jitter_mixed = false;
};

// tw2de_fsr3 is on (deko3d only).
bool Fsr3On();
// The jitter of this frame's scene draws in normalized device coordinates (added to clip x, y times w), and the frame
// boundary (render/draw.cpp's frame start): the next jitter of the sequence for a render of 'width' x 'height' to an
// output of 'out_width' x 'out_height' (the last frame's sizes).
void Fsr3FrameStartLocked();
void Fsr3JitterNdc(float out[2]);
// tw2de (2026-11-08): tw2de_fsr3_diag's test switches (the fsr3_view file's fourth number; 0 without it).
uint32_t Fsr3DiagFlags();
// The game's upscale draw: FSR 3.1 from the picture at image id 'color_image' (its post-processed scene, 'width' x
// 'height') into 'target' (its host size is the output size), when the scene inputs are there. True: done (the target
// holds the upscaled picture; the draw is not made). Leaves no state of the draw bound (the caller rebinds).
bool Fsr3UpscaleLocked(uint32_t color_image, uint32_t width, uint32_t height, HostSurface *target,
                       const Fsr3SceneInputs &scene, bool copy_to_target = true, uint32_t out_width = 0,
                       uint32_t out_height = 0);
// tw2de (2026-11-08): out_width / out_height (with copy_to_target false): the output's size instead of the target's
// (the target then only gives the format).
// tw2de (2026-11-08): with copy_to_target false the output stays in FSR's own texture: copied from there (its
// pixels 'box') into a texture's level 0 at (x, y); its layouts handled. False: no output this frame.
bool Fsr3CopyOutputLocked(plume::RenderTexture *dest, uint32_t x, uint32_t y, const plume::RenderBox &box);

}  // namespace renut::gpu

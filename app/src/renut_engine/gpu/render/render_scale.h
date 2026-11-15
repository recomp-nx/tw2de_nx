/**
 * @file    gpu/render/render_scale.h
 * @brief   Render resolution: host pixels per guest pixel.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace renut::gpu {

// Host pixels per guest pixel of a render target, and of what resolves from
// it, as one of a fixed set of ratios: the render resolutions
// (renut_resolution: 1x, 480p, 1080p, 1440p, 4K) and the targets that render
// finer than the rest (graphics_options.h: shadow maps at 4x, ambient
// occlusion at twice the resolution: 4/3, 6x). Code 0 is 1x, the scale of all
// content that is not rendered (guest textures). Shaders get the code in the
// descriptor index (kResolutionScaleShift) and have the same factors
// (renutTextureScale in shader_common.h).
// tw2de (2026-11-07, dynamic resolution): the fraction of a dynamic-resolution target's host size drawn this frame
// (render/dynamic_resolution.cpp, set at the frame's start; 1 without it).
extern float g_drs_factor;

struct RenderScale {
  // tw2de (2026-11-08): 6/5, 15/8 and 5/4 (codes 8-10): a 1600x900 / 1024x576 scene's post-processing at a 1920x1080
  // output, a 1024x576 one's at 1280x720 (FSR 3.1 before it, graphics_options.cpp). Deko3d only: a descriptor index carries 3 bits of the code
  // (kResolutionScaleShift, masked), which only the Vulkan shaders read (deko3d's: pass scale1, no scale divisor).
  // tw2de (2026-11-11): 4/5 (code 11): a 1600x900 scene's GUI target at a 1280x720 output (the GUI drawn at the
  // output's size, draw.cpp).
  // TW2 (2026-11-14): dynamic resolution's levels as scales of their own (codes 12-32: (80 - 2 n) / 80, n = 0-20, of
  // a 720-line scene; dynamic_resolution.cpp DrsSceneScale). The scene's targets and what resolves from them are made
  // at the level's size and drawn whole, so no shader knows of it. The way of 2026-11-07 ('drs' below: targets of the
  // full size drawn in part, the texture coordinates scaled in the shaders) cost TW2 a variant of every
  // shader that samples such a texture and 8 ms of GPU a frame in its arithmetic (the Tutorial scene, 32.9 ms).
  static constexpr uint32_t kFixedCount = 12;
  static constexpr uint32_t kDrsFirst = 12;
  static constexpr uint32_t kDrsLevels = 21;
  static constexpr uint32_t kCount = kDrsFirst + kDrsLevels;
  static constexpr uint32_t kNumerators[kCount] = {1,  2,  3,  2,  3,  4,  6,  4,  6,  15, 5,  4,  80, 78, 76, 74, 72,
                                                   70, 68, 66, 64, 62, 60, 58, 56, 54, 52, 50, 48, 46, 44, 42, 40};
  static constexpr uint32_t kDenominators[kCount] = {1,  3,  2,  1,  1,  1,  1,  3,  5,  8,  4,  5,  80, 80, 80, 80, 80,
                                                     80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80};

  uint32_t code = 0;
  // tw2de (2026-11-07, dynamic resolution): a scene target whose host texture keeps its size ('code' times the guest
  // one) while only g_drs_factor of it is drawn from its top-left corner: viewports, scissors, clears and resolves are
  // scaled by it (Coord, DrawFactor), and the shaders that sample what is resolved from it scale their texture
  // coordinates by it too (the descriptor index's kDrsTextureBit, SharedConstants::drs_fill).
  bool drs = false;

  // Host pixels per guest pixel of the host texture (its size).
  float Factor() const { return float(kNumerators[code]) / float(kDenominators[code]); }
  // tw2de: host pixels per guest pixel as drawn this frame.
  float DrawFactor() const { return drs ? Factor() * g_drs_factor : Factor(); }
  bool operator==(const RenderScale &) const = default;
  // TW2 (2026-11-14): one of dynamic resolution's levels (kDrsFirst).
  bool Dynamic() const { return code >= kDrsFirst; }
  // tw2de: code and drs in one number (keys).
  uint32_t Key() const { return code | (drs ? 64u : 0u); }  // TW2 (2026-11-14): 64 (codes up to 32; was 16)

  // A guest pixel edge in host pixels as drawn, to the nearest: rects that share an edge in guest pixels share it in
  // host pixels.
  int32_t Coord(int32_t guest) const {
    const double host = double(guest) * kNumerators[code] / kDenominators[code];
    return int32_t(std::lround(drs ? host * double(g_drs_factor) : host));
  }

  // A guest size in host pixels of the host texture (dynamic resolution aside), 1 at least.
  uint32_t Size(uint32_t guest) const {
    return std::max<uint32_t>(uint32_t(std::lround(double(guest) * kNumerators[code] / kDenominators[code])), 1u);
  }
  // tw2de: a guest size in host pixels as drawn this frame, 1 at least.
  uint32_t DrawnSize(uint32_t guest) const { return std::max<uint32_t>(uint32_t(Coord(int32_t(guest))), 1u); }

  // The scale of ratio numerator / denominator, if it is one of them.
  static bool FromRatio(uint32_t numerator, uint32_t denominator, RenderScale &out) {
    for (uint32_t c = 0; c < kFixedCount; ++c) {  // TW2 (2026-11-14): not dynamic resolution's levels
      if (uint64_t(numerator) * kDenominators[c] == uint64_t(kNumerators[c]) * denominator) {
        out.code = c;
        return true;
      }
    }
    return false;
  }

  // This scale times 'multiplier', or this one when that is not among them.
  RenderScale Times(uint32_t multiplier) const {
    RenderScale out = *this;
    FromRatio(kNumerators[code] * multiplier, kDenominators[code], out);
    return out;
  }
};

} // namespace renut::gpu

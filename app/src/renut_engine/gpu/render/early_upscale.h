// tw2de (2026-11-10): the scene's resolves upscaled before the post-processing without FSR 3.1 (early_upscale.cpp).
#pragma once

#include <cstdint>

namespace renut::gpu {

struct HostTexture;
struct HostSurface;

// Draws src's picture (its top-left src_w x src_h texels) into dst's top-left dst_w x dst_h with Lanczos-2, or FSR 1's
// EASU with 'easu' (the boot menu's Upscaler: FSR1; no RCAS), alpha kept,
// on the render thread's command list. True when recorded: the caller binds its targets, pipeline, layout and viewport
// again.
bool EarlyUpscaleLocked(HostTexture *src, uint32_t src_w, uint32_t src_h, HostTexture *dst, uint32_t dst_w,
                        uint32_t dst_h, bool easu);
// tw2de (2026-11-11): src's whole picture (a single-sampled colour target) into dst's whole picture, Lanczos-2.
bool EarlyUpscaleSurfaceLocked(HostSurface *src, HostSurface *dst);
// TW2 (2026-11-14): src's top-left src_w x src_h host pixels copied into a texture of the pass's own, then drawn over
// dst's whole picture (Lanczos-2, or EASU with 'easu'). src and dst may be one surface: the game's last pass drawn at
// the scene's size into the corner of the output's target, then enlarged over it. True when recorded: the caller
// binds its targets, pipeline, layout and viewport again.
bool EarlyUpscaleCopyLocked(HostSurface *src, uint32_t src_w, uint32_t src_h, HostSurface *dst, bool easu);

}  // namespace renut::gpu

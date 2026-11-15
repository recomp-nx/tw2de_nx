// tw2de (2026-10-25, tw2de_renut_upscale_filter 3): FSR 1's RCAS pass after the upscale draw (fsr1.cpp).
#pragma once

namespace renut::gpu {

struct HostSurface;

// Records RCAS on 'surface' (the upscale draw's 8-bit RGBA target, single-sampled) on the render thread's command list:
// a copy of the picture, then RCAS from the copy into the surface (its alpha kept). At most once a frame. True when it
// recorded it: the caller binds its targets, pipeline, layout and viewport again.
bool Fsr1RcasRunLocked(HostSurface *surface);

}  // namespace renut::gpu

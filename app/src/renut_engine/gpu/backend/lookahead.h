// TW2 (2026-11-14, tw2de_lookahead): what a queued render thread item does to resolve destinations.
//
// A resolve whose destination is left in its target's image (the depth and colour hand-overs, render/draw.cpp) has to
// be drawn after all when the target is written while the destination still holds that content. The game often
// overwrites such a destination a few draws later without ever sampling it (a scene resolve before the hair pass that
// nothing reads, the depth resolve before the forward pass, the bloom chain's ping-pong). The render thread can tell:
// the items after the one it is running are in its queue. Each draw, resolve and clear queued by the game thread
// carries a Tag; the render thread reads the tags ahead (render_thread::PeekTag) and gives the content up as dead when
// the first item that touches the destination is a resolve that overwrites all of it. Anything it cannot read (an
// untagged item, a draw whose shaders are unknown, the end of the queue) means "alive": the copy is made as before.
#pragma once

#include <cstdint>

namespace renut::gpu::lookahead {

struct Tag {
  enum Kind : uint8_t { kUnknown = 0, kNone = 1, kDraw = 2, kResolve = 3 };
  uint8_t kind = kUnknown;
  uint8_t count = 0;  // kDraw: pages[] in use
  // kResolve: an explicit source rect; destination x, y, level and slice all 0.
  bool has_rect = false, origin = false;
  int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;        // kResolve: the source rect (has_rect)
  uint32_t source_width = 0, source_height = 0;  // kResolve: the source surface's size in guest pixels (0: unknown)
  // kDraw: the base pages (GPU addresses >> 12) of the textures its shaders can fetch, as the device has them when
  // the draw is captured. kResolve: the destination header's six fetch words (TextureFetch::FromHeader).
  uint32_t pages[26] = {};
};

// An item that samples no texture and writes no resolve destination (a clear).
extern const Tag kNoneTag;

// Game threads. The fetch constants a shader's texture fetches name (bit r: constant r), noted when the shader is
// created or registered; a shader not noted makes its draws kUnknown.
void NoteShaderFetches(uint32_t shader_va, uint32_t fetch_mask);
// The tag of a draw from the device bytes it is captured from.
void FillDrawTag(Tag &tag, const uint8_t *device);

}  // namespace renut::gpu::lookahead

/**
 * @file    gpu/backend/guest_gpu_memory.h
 * @brief   tw2de (2026-10-30): the GPU's mapping of all of guest physical memory (tw2de_memexport_zero_copy).
 *
 * The game's memexport kernels (its texture pool's compaction) write guest memory. Until now the renderer gave them
 * a window in a GPU buffer of its own: seeded from guest memory before the draw (32 MB a window, 7 ms with three
 * threads), its results copied back to guest memory after the slot's fence (by the write mask, 19 ms for 65 MB), and
 * two 128 MB export buffers, seed blocks and a readback to hold it all. The probe (guest_gpu_probe.cpp, 2026-10-30)
 * showed that a deko3d memory block can be made over the code alias of a guest memory chunk, that the GPU reads and
 * writes it coherently with the usual cache maintenance, and that the kernel refuses to map or unmap a view of such
 * memory afterwards (0xD401), so the views must be mapped first.
 *
 * So, once, at the first frame: every chunk of physical memory is committed (sdk guest_memory_switch.cpp places their
 * shadows at a fixed offset of one reservation, so the 512 MB are one range of addresses), every view of them is
 * mapped, one memory block is made over the whole shadow, and the range is pinned (RexGmPin: protection changes are
 * recorded instead of made). The kernels then write guest memory itself, at the stream's physical address; a window
 * costs the CPU a cache clean of the pages the game wrote since the last one (so the GPU reads them) before the
 * draw, and a clean-and-invalidate of the pages the kernels wrote (the page flags, a small buffer a slot) after the
 * slot's fence, so the CPU reads the results. No seed, no copy back, no export buffers. The same mapping lets the
 * textures be untiled from guest memory on the GPU.
 *
 * What stays as it was when the mapping cannot be made (the heap, the kernel's limits, deko3d): everything, with a
 * log line saying why; tw2de_memexport_zero_copy = false keeps the old path on purpose.
 */
#pragma once

#include <cstdint>

namespace plume {
struct RenderBuffer;
}

namespace renut::gpu {

struct VideoState;

namespace guest_gpu {

// Once a frame on the render thread with State().mutex held (draw.cpp AdvanceStreamCacheFrameLocked): makes the mapping
// at the first frame when the cvar asks for it (a one-time cost at the start, logged).
void FrameLocked(VideoState &s);

// The mapping exists: the export windows are guest memory itself.
bool On();

// The buffer over all of physical memory: its byte 'physical' is guest physical address 'physical'. Null when off.
plume::RenderBuffer *Buffer();

// Before the GPU reads [physical, physical + size): the CPU's lines of it cleaned to memory. With the store cards on,
// only the pages the game wrote since their last clean (the cards; HarvestPageLocked); every page otherwise.
void CleanForGpu(uint32_t physical, uint32_t size);
// The same, every page, whatever the cards say: after a write of the renderer's own into guest memory, which marks no
// card (the language pack's glyphs and buttons patched over the game's textures, textures.cpp).
void CleanForGpuNow(uint32_t physical, uint32_t size);

// After the GPU wrote [physical, physical + size) (the slot's fence waited): the CPU's lines of it dropped, so the CPU
// reads what the GPU wrote. A line the game wrote meanwhile is written back first (dc civac), as on the 360 (the game
// keeps the CPU and the GPU out of each other's lines).
void InvalidateAfterGpu(uint32_t physical, uint32_t size);

// The GPU's L2 flushed and invalidated before it reads guest memory the CPU may have written since it last read it
// (nothing when the mapping is GPU-uncached). Once per draw at most: the first call of a draw records the barrier
// (a full one, which ends any render pass: true then, so the caller forgets its bound framebuffer).
bool InvalidateGpuCachesLocked(VideoState &s);
void NewDrawLocked();

// For the [burst] line: the frame's cleans and invalidates (bytes, pages skipped by the cards, milliseconds).
struct Stats {
  uint64_t clean_bytes = 0, clean_skipped_pages = 0, invalidate_bytes = 0;
  double clean_ms = 0.0, invalidate_ms = 0.0;
};
Stats TakeStats();

}  // namespace guest_gpu

// render/draw.cpp, for guest_gpu::CleanForGpu: how many times the store cards of a physical page were found written by
// the game or a host writer (the cards read and reset now: HarvestPageLocked there), and the current interval
// (g_flush_serial).
uint32_t GuestPageWriteCount(uint32_t page);
uint32_t GuestWriteInterval();
// tw2de (2026-10-30): the counts of every page (indexed by physical page; read after HarvestGuestPagesLocked has
// taken the cards of the pages in question, eight a load where they are clean: a 32 MB window's 8192 pages in a few
// microseconds, where one GuestPageWriteCount a page was 0.5 ms).
const uint32_t *GuestPageWriteCounts();
void HarvestGuestPagesLocked(uint32_t first_page, uint32_t last_page);

}  // namespace renut::gpu

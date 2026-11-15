/**
 * @file    gpu/backend/guest_gpu_probe.h
 * @brief   tw2de (2026-10-30): can the GPU read and write the guest's memory itself?
 *
 * The memexport kernels (the game's texture copy kernels) write guest memory. The renderer gives them a window in a
 * GPU buffer instead: seeded from guest memory before the draw, its results copied back to guest memory after the
 * slot's fence (render/draw.cpp). Both copies go away if the GPU can address the guest's physical memory: on Horizon it
 * is committed as heap memory mapped as a code alias (sdk guest_memory_switch.cpp: svcMapProcessCodeMemory, then RW),
 * mirrored into the 360's views with svcMapProcessMemory. Whether deko3d can make a memory block over such a code alias
 * (the kernel's device mapping of memory in the AliasCodeData state), whether the GPU then sees the CPU's writes and
 * the CPU the GPU's with the usual cache maintenance, and what the kernel does to the views' mappings while the memory
 * is mapped to the device, is unknown until tried on a console: this is the trial.
 *
 * tw2de_diag_gpu_guest_probe = 1: a 2 MB region of its own, made exactly as a guest memory chunk is (two 1 MB heap
 * blocks mapped as code aliases next to each other, RW), mapped for the GPU as one buffer; the GPU copies out of it and
 * into it, the CPU checks both; a view is mapped on it, read, its permission changed and unmapped while the GPU mapping
 * exists (what RexGmFaultIn and RexGmProtect would do). = 2: also a committed chunk of the real guest memory, its views
 * mapped first, GPU-mapped and read (never written). Everything is logged as [gpuprobe] lines in the game log and in
 * deko3d.log (written at once: the last line before a crash says where it died). Default 0: off.
 */
#pragma once

namespace renut::gpu {

// Once a frame on the render thread with State().mutex held (draw.cpp AdvanceStreamCacheFrameLocked): runs the probe
// at its frame and cleans up a few frames later. Nothing when the cvar is 0 or not on deko3d.
void GuestGpuProbeFrameLocked();

}  // namespace renut::gpu

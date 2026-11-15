// tw2de (2026-11-05, tw2de_diag_vsync_timing): each frame's path measured against the display's refresh, to find
// the frames shown a refresh too long (a drop at 30 fps that the average frame rate does not show) and the stage that
// made them late, or the frames done so early that they wait (latency). renut_engine/frame_timing.cpp.
//
// Per guest Swap: the game's frame start (the vblank its limiter woke it at), the Swap, the render thread's start, the
// swapchain image acquired, the work submitted, the present returned, and the GPU's end of the frame (a waiter thread
// on the frame's fence). Each refresh's time comes from the display's vsync event (display_sync.cpp). Off: one relaxed
// load per call.
#pragma once

#include <cstdint>

namespace plume {
struct Deko3DFenceCopy;
}

namespace tw2de::frame_timing {

enum Stage : uint32_t {
  kSwap = 0,       // the game's Swap (its CPU frame done)
  kRenderBegin,    // the render thread takes the frame
  kRenderLocked,   // its present closure has the video mutex (the front buffer looked up next)
  kRenderReady,    // the closure's own work done, Video::Present called
  kPresentLocked,  // Video::Present has the video mutex
  kAcquireBegin,   // asks the swapchain for an image
  kAcquired,       // has one
  kSubmitted,      // the frame's work submitted
  kPresented,      // the swapchain's present returned
  kStageCount
};

bool On();

// The display refreshed (its 'index'-th refresh) at 'ns' (steady_clock): the vsync thread.
void NoteRefresh(uint64_t index, uint64_t ns);
// The game's vblank handler ran; the device's vblank counter is now 'counter' (the vblank pump).
void NoteVblank(uint32_t counter);
// The game's limiter wakes its main thread for the next frame at vblank 'target' (SwapInterrupt, at Swap 'swap').
void NoteTarget(uint64_t swap, uint32_t target);
// A stage of Swap 'swap', now.
void Mark(uint64_t swap, Stage stage);
// The render thread: the Swap whose frame the next present ends (stages kAcquireBegin..kPresented use it).
void SetPresenting(uint64_t swap);
uint64_t Presenting();
// tw2de (2026-11-05, second round): the render thread's own time for the frame. At the start of its present: its busy
// and blocked ms for the frame's work so far (render_thread::BusyMsThisFrame / WaitedMsThisFrame); before the acquire:
// the blocked ms then (the difference is what the present waited for before asking for an image).
void NoteRenderThread(uint64_t swap, double busy_ms, double waited_ms);
void NoteWaitedBeforeAcquire(uint64_t swap, double waited_ms);
// After Video::Present: the present's index (Video::PresentCount) for the GPU waiter.
void NotePresentIndex(uint64_t swap, uint64_t present_index);

// tw2de (2026-11-05, fourth round): the render thread's frame bookkeeping (the present closure's work besides the
// present) by part. Run 3 found 15-37 ms spikes in it, the thread running all the while. PartScope adds a part's wall
// and CPU time to the calling thread's sums; TakeParts(swap), at the closure's end, takes them for that Swap: a
// [vsync] parts line when they come to 5 ms or more, and each part's mean/worst in the 10 s line.
enum Part : uint32_t {
  kPartFrontBuffer = 0,  // FrontBufferImageLocked and the black-frame check
  kPartTexUnlocks,       // AdvanceTextureFrameLocked: the unlocked textures
  kPartTexIdleEvict,     // ... the idle textures released (every 120 frames)
  kPartTexColdEvict,     // ... the cold ones under the memory reserve (every 32 frames)
  kPartStreamStart,      // AdvanceStreamCacheFrameLocked: trace, guest GPU probe/mapping, hook and CPU-read reports
  kPartStreamBurst,      // ... the [burst] stats
  kPartStreamSweep,      // ... card writers, watch, idle export buffers, unused targets, cards verify
  kPartStreamVerify,     // ... the stream and index verifiers (stale copies)
  kPartStreamIdleDrop,   // ... the idle cached streams and index ranges dropped (every 60 frames)
  kPartLatch,            // LatchResolutionScaleLocked
  kPartMemoryReport,     // MaybeReportMemory
  kPartCount
};
class PartScope {
 public:
  explicit PartScope(Part part);
  ~PartScope();
  PartScope(const PartScope &) = delete;
  PartScope &operator=(const PartScope &) = delete;

 private:
  Part part_;
  uint64_t t0_ = 0, cpu0_ = 0;
};
void TakeParts(uint64_t swap);

// tw2de (2026-11-05, progressive submit): the frame of Swap 'swap' went to the GPU in 'pieces' lists at its kicks
// ('kicks' of them) and its last list at the present; 'draws' in all, 'tail_draws' in that last list; 'submit_ms': the
// render thread's time ending and submitting lists (the kicks' and the present's). backend/video.cpp.
void NotePieces(uint64_t swap, uint32_t pieces, uint32_t kicks, uint32_t draws, uint32_t tail_draws, double submit_ms);

// tw2de (2026-11-05, sixth round): what the display actually showed, not the model above. Run 7bc02322: the model's
// "ready" sat 0.05 ms (mean) before the refresh it was credited to, for nearly every frame: the frame's end is tied to
// a vsync (the swapchain image's release), so a frame the model counts as on time can be a refresh late on screen.
// At the present's acquire the queue gets a fence before the acquire and one after its wait for the image's release
// (Deko3DSignalNow). When the second comes measurably after the first, the GPU waited for the release: that time is
// when the display stopped showing the image's previous frame, i.e. when the frame after it went on screen. From two
// such times a frame's real time on screen in refreshes; [vsync] seen lines for a frame shown longer than the swap
// interval. 'slot': the swapchain image.
void NoteAcquire(uint64_t swap, uint32_t slot, const plume::Deko3DFenceCopy &before, const plume::Deko3DFenceCopy &after);

}  // namespace tw2de::frame_timing

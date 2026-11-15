// tw2de (2026-10-19, latency batch 3): the frame limiter locked to the display's refresh, and the input -> display
// measurement (renut_engine/display_sync.cpp).
//
// On the Switch the compositor shows a new image only at its 60 Hz refresh (there is no tearing mode; swap interval 0
// only stops the present from waiting). The frame limiter (frame_cap) released the game's frames on its own 33.3 ms
// timer, at a phase to the refresh that slid as the two clocks drifted: an image finished just after a refresh waited
// almost 16.7 ms more, and now and then one was shown for three refreshes and the next for one. Here a thread follows
// the display's vsync event (viGetDisplayVsyncEvent): the limiter releases each frame at a refresh plus a phase, and
// the phase is found by measurement (tw2de_display_phase_auto): as late as the frame's image still makes the refresh it
// made before. A frame that ends late is released at once as before (no snapping to 20 fps).
#pragma once

#include <cstdint>
#include <string>

namespace tw2de::display_sync {

// Starts the thread on the first call (the Switch); a no-op elsewhere and after the first call. tw2de (2026-10-27):
// always started (the game's vblank pump follows it); tw2de_display_sync adds the frame limiter's part.
void Start();

// tw2de (2026-10-27): the display's refresh count once it differs from 'seen' (the call waits for the next refresh),
// or as it is after 'timeout_ns' with none. 0 forever where there is no thread (not the Switch, no vsync event).
uint64_t WaitRefresh(uint64_t seen, uint64_t timeout_ns);

// tw2de (2026-10-29): the display's refresh period in ns, as measured (averaged over about a second); 0 when it is
// not known (not the Switch, no refresh in the last second).
uint64_t RefreshPeriodNs();

// The next frame's release for the frame limiter, in std::chrono::steady_clock nanoseconds: the frame now ending at
// 'now', the previous one released at 'previous' (0: none), 'frame_ns' the limiter's frame time. 0 when the display's
// refresh is not known (the limiter keeps its own timer then). '*anchored': the release is a refresh plus the phase.
uint64_t NextRelease(uint64_t now, uint64_t previous, uint64_t frame_ns, bool *anchored);

// The limiter released the game's next frame (its Swap is rex::latency::SwapCount() + 1) at 'at'.
void NoteRelease(uint64_t at, bool anchored);

// The renderer presented the image of the guest's 'swap'-th Swap (after Video::Present).
void NotePresented(uint64_t swap);

// One line for the 10 s report; empty when the thread does not run.
std::string Report();

}  // namespace tw2de::display_sync

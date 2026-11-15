// tw2de (2026-10-25): frame_interval.h.
//
// The game paces itself: its swap callback (device.cpp SwapInterrupt) sets the vblank its vblank callback waits for to
// wake the main thread, D3DRS_PRESENTINTERVAL vblanks after the last swap (the game asks for 2: 30 fps), and Renut's
// PaceSwap holds Swap to the same grid. Its frame step is not fixed (checked in play: the game keeps its speed at any
// frame rate), so the lock can go: with tw2de_frame_interval 1-4 the game is told "immediate" (no wait of its own) and
// the display's swap interval sets the rate (1 = every refresh: 60 fps at 60 Hz, 2 = 30, 3 = 20, 4 = 15; at 120 Hz 4 is
// 30). A prerendered movie plays at its own 30 fps: while one is open the game's lock was put back at 2 and the display
// showed every refresh (interval 1); since 2026-10-29 the display's swap interval 2 holds it to 30 fps (SwapInterval).
// tw2de (2026-10-27): the rate set by the game's own limiter instead (its frame interval: 1-4 vblanks of its 60 Hz
// pump), the display's interval at 1 (since 2026-10-29 at the frame interval too: SwapInterval). With the display's interval setting the rate, a game faster than it ran ahead
// until the queues were full (render_thread: 1 frame; the swapchain: 3 images, 2 waiting for the display), so each
// frame's input was read up to three frames before it was shown. The game's limiter holds the main thread itself, before
// it reads input: nothing waits in a queue, as with the game's own 30 fps lock.
#include "renut_engine/frame_interval.h"
#include "renut_engine/display_sync.h"  // tw2de (2026-10-29): the refresh rate, for movies

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>

// TW2 (2026-11-14): 1-4, default 2 (was 0-4, 0 = "the game's lock": PaceSwap in device.cpp held each Swap to the
// presentation interval the game asked D3D for, as the Xbox 360's GPU did). TW2 has no limiter in its engine
// (60.0 fps in its movies and menus at interval 1, tw2de_226.log), so the swap interval below is the whole frame limiter.
REXCVAR_DEFINE_INT32(tw2de_frame_interval, 2, "Game/Enhancements",
                     "The frame rate as display refreshes a frame (the boot menu's \"Frame interval\"): the display's "
                     "swap interval, the game's only frame limiter. 1 = every refresh (60 fps at 60 Hz), 2 = every second "
                     "(30), 3 (20), 4 (15)")
    .range(1, 4);

namespace rex::system {
int OpenMovieFiles();  // sdk/src/system/xfile.cpp
}

namespace tw2de::frame_interval {

namespace {
int Setting() {
  return std::clamp(REXCVAR_GET(tw2de_frame_interval), 1, 4);
}

// tw2de (2026-10-29): a display at 60 Hz (59-61): a movie paced by the display's swap interval 2 (below).
// tw2de (2026-11-07): and at 90 Hz (89-91) interval 3, at 120 Hz (118-122) interval 4: 30 fps the same way.
// 0: another rate (or not measured yet).
uint32_t MovieSwapIntervalFor(uint64_t period) {
  if (period >= 16'393'443ull && period <= 16'949'153ull)
    return 2u;
  if (period >= 10'989'011ull && period <= 11'235'955ull)
    return 3u;
  if (period >= 8'196'722ull && period <= 8'474'577ull)
    return 4u;
  return 0u;
}
uint32_t MovieSwapInterval() {
  return MovieSwapIntervalFor(tw2de::display_sync::RefreshPeriodNs());
}

void NoteMovie(bool playing) {
  static std::atomic<int> last{-1};
  if (last.exchange(playing ? 1 : 0) == (playing ? 1 : 0))
    return;
  if (!playing) {
    REXLOG_INFO("[interval] no movie: the frame interval from tw2de_frame_interval");
    return;
  }
  const uint64_t period = tw2de::display_sync::RefreshPeriodNs();
  if (const uint32_t n = MovieSwapIntervalFor(period))
    REXLOG_INFO("[interval] movie: the display at every {} refreshes while it plays (30 fps; display {:.2f} Hz)", n,
                1e9 / double(period));
  else
    REXLOG_INFO("[interval] movie: the game at the display's rate (1 vblank a frame), every refresh shown (display {})",
                period ? fmt::format("{:.2f} Hz, not 60, 90 or 120", 1e9 / double(period))
                       : std::string("refresh not known"));
}
}  // namespace

bool MoviePlaying() {
  return rex::system::OpenMovieFiles() > 0;
}

// tw2de (2026-10-29): a prerendered movie (with a frame interval set): the game's limiter at 1 vblank and the
// display's swap interval at 2. The movie keeps its own 30 fps and the game shows the newest of its frames. Sampled at
// 30 fps by the game's own limiter, at a phase to the movie's frames that the display's refresh fixed (since 2026-10-27;
// random each run before, on a timer), a game frame near a movie frame's change took it or the one before by chance:
// frames shown twice, others skipped. At 60 fps that was gone but for jumps at the same places of a movie, and the GPU
// drew every movie frame twice. Now the game may run at 60 but the display takes an image every second refresh, so
// the game is held to 30 by the display itself (its present waits for a free image). Checked on the console (A/B of
// three ways): smooth through the places that jumped, the frame time graph flat, half the GPU work of 60 fps. Only on a
// 60 Hz display (DisplayAt60: a refresh rate changed by an overlay, 40-55 Hz handheld, would make interval 2 too slow).
// At another refresh the game runs at the display's rate during the movie, every refresh shown: each of the movie's
// 30 frames a second is shown (at 50 Hz for 2 refreshes or 1); the game's 30 fps lock counts 2 of its vblanks, which
// follow the display, so it gave 25 fps at 50 Hz (checked on the console: 50.30 Hz measured, 25 fps).
uint32_t GameVblanks() {
  // tw2de (2026-10-29): movies: 1 vblank a frame. At 60 Hz the display's swap interval 2 holds the game to 30
  // fps; at another refresh it runs at the display's rate (above).
  if (MoviePlaying())
    return 1u;
  return uint32_t(Setting());
}

uint32_t SwapInterval(bool vsync) {
  // tw2de (2026-10-29): while a movie plays, every second refresh on a 60 Hz display, every refresh on another
  // (above).
  NoteMovie(MoviePlaying());
  if (MoviePlaying()) {
    const uint32_t n = MovieSwapInterval();  // tw2de (2026-11-07): 2 at 60 Hz, 3 at 90, 4 at 120
    return n ? n : 1u;
  }
  // tw2de (2026-10-29): in play, the frame interval: the display holds each image that many refreshes. The game's
  // limiter still sets the rate (GameVblanks: no frames queue up, the input as before). With interval 1 a frame that
  // finished near a refresh was shown at that one or the next by chance: uneven frames when the game was fast (a
  // higher CPU clock made it worse). Checked on the console: frame pacing even at the higher CPU clock.
  // TW2 (2026-11-14): this is the frame limiter (no game lock, no PaceSwap). renut_vsync off: no wait at all.
  return vsync ? uint32_t(Setting()) : 0u;
}

namespace {
std::atomic<uint64_t> g_last_vblank_us{0}, g_late_deadline_us{0};
std::atomic<bool> g_late_start{false};
uint64_t SteadyUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
}  // namespace

void NoteGameVblank() {
  g_last_vblank_us.store(SteadyUs(), std::memory_order_relaxed);
}

void NoteLateStart() {
  // The vblank the frame would have waited for: the game's next one, a 60 Hz period after the last (the pump follows the
  // display's refresh; its lead keeps the period).
  const uint64_t last = g_last_vblank_us.load(std::memory_order_relaxed);
  const uint64_t now = SteadyUs();
  uint64_t next = last ? last + 16667 : now;
  while (next < now)
    next += 16667;
  g_late_deadline_us.store(next, std::memory_order_relaxed);
  g_late_start.store(true, std::memory_order_release);
}

uint64_t LateLoadDeadlineUs() {
  return g_late_deadline_us.load(std::memory_order_relaxed);
}

bool TakeLateStart() {
  g_late_deadline_us.store(0, std::memory_order_relaxed);
  return g_late_start.exchange(false, std::memory_order_acq_rel);
}

}  // namespace tw2de::frame_interval

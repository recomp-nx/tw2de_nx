// TW2 (2026-11-12): the black frames in the movies and menus (the render thread's refresh frames).
//
// TW2's render thread (its loop sub_82EEC008) draws a frame of its own when the game's next frame has not come
// 50 ms (0.05 s, the double at 0x82251400) after the last one started (the renderer's +0x268, sub_82EEB7D0) and again
// every 50 ms after: a "refresh" frame for a stalled game (a long load), so the GUI on top (a loading icon, subtitles)
// keeps moving. Its frame info has byte +0x2416 set (+0x2426 from the info pointer the frame function gets), and the frame
// function sub_82EEAF48(renderer r3, frame info r4, r5) then draws only the GUI over a blurred copy of the last scene
// target (44CE5790 here, sub_82EE57F8). A prerendered movie, the main menu's background movie included, is drawn by the
// GUI pass, not into that target: the copy is black, and the refresh frame shows one game frame (two refreshes) of
// black between two frames of the movie. On the Xbox 360 the game's frames came every 33 ms. Here a frame now and then
// comes 50-55 ms after the one before (the movie's decoder or the job workers late: checked with tw2de_diag_frames, the
// render thread had waited 27-40 ms for it), and every such frame was a black one in the recordings.
// So a refresh frame is drawn only once the game has sent no frame for tw2de_refresh_frame_ms (250 ms by default: a real
// stall, the game's own behaviour from there, every 50 ms); before that the call is left out and the last frame stays on
// the screen until the game's next one. A refresh frame is recognised by its caller (the return address 0x82EEC308, the
// call in the render loop's refresh block), with either value of the info's byte; the game's frames come from
// 0x82EF1744 (its render-frame command) and the loading screen's from 0x82EEBFD0 (sub_82EEBE88), which are left alone.
// tw2de_diag_frames: a log line a call (the time since the last call's start and end).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"

REXCVAR_DECLARE(bool, tw2de_diag_frames);  // renut_engine/gpu/d3d/hooks/device.cpp
REXCVAR_DECLARE(bool, tw2de_frame_dump);   // renut_engine/gpu/render/draw.cpp
// 2026-11-12 (user: black frames again on the fade from black into the main menu, a load there holds the game's frames
// back longer than 250 ms): -1 by default, no refresh frame at all; the last frame stays on the screen through a stall.
REXCVAR_DEFINE_INT32(tw2de_refresh_frame_ms, -1, "Game/Enhancements",
                     "The render thread's own refresh frame (only the GUI over a blurred last scene: black during a "
                     "movie) once the game has sent no frame for this long; -1 never (the last frame stays), 0 the "
                     "game's 50 ms")
    .range(-1, 10000);

REX_EXTERN(__imp__sub_82EEAF48);

namespace {
constexpr uint32_t kRefreshCall = 0x82EEC308;  // the return address of the refresh block's call
uint32_t Byte(const uint8_t *base, uint32_t va) { return va ? base[va] : 0xFFu; }
// The render thread only (the frame function's one caller thread).
std::chrono::steady_clock::time_point g_last_start, g_last_end, g_last_game_frame;
double g_last_took = 0;
uint64_t g_skipped = 0;
}  // namespace

extern "C" REX_FUNC(sub_82EEAF48) {
  const uint32_t info = ctx.r4.u32, caller = uint32_t(ctx.lr);
  const auto now = std::chrono::steady_clock::now();
  // 2026-11-14: every frame of the refresh block, whatever the info's byte. The block clears the byte (0x82EEC2B8, no
  // blurred copy under the GUI) while the renderer's vtable +0x84 or +0x88 answers 1: during a fade those frames are
  // black too, and the fade into the main menu, where the game's frames come 66 ms apart in some runs, showed one
  // between every two game frames for 2-3 s (the user's recording of 2026-11-14; runs/f1_session.log has the same
  // G R G R chain with the byte 0 at its menu fade), the jail scene one at a late frame during a fade.
  const bool refresh = caller == kRefreshCall;
  if (!refresh)
    g_last_game_frame = now;
  const int32_t min_ms = REXCVAR_GET(tw2de_refresh_frame_ms);
  if (refresh && (min_ms < 0 || (min_ms > 50 && now - g_last_game_frame < std::chrono::milliseconds(min_ms)))) {
    ++g_skipped;
    if (REXCVAR_GET(tw2de_diag_frames))
      REXLOG_INFO("[frames] render: refresh frame left out (info+2426 {}; {:.1f} ms after the game's last frame; {} so "
                  "far)",
                  Byte(base, info + 0x2426), std::chrono::duration<double, std::milli>(now - g_last_game_frame).count(),
                  g_skipped);
    return;
  }
  if (REXCVAR_GET(tw2de_diag_frames)) {
    static std::atomic<uint32_t> lines{0};
    const double since_start = std::chrono::duration<double, std::milli>(now - g_last_start).count();
    const double since_end = std::chrono::duration<double, std::milli>(now - g_last_end).count();
    if (lines.fetch_add(1) < 30000)
      REXLOG_INFO("[frames] render: info+2426 {} info+2427 {} lr {:08X}: {:.1f} ms after the last start, {:.1f} ms after "
                  "its end (it took {:.1f} ms)",
                  Byte(base, info + 0x2426), Byte(base, info + 0x2427), caller, since_start, since_end, g_last_took);
    // With tw2de_frame_dump: a dump of the first refresh frame drawn 40 s after the start.
    static const auto start = now;
    static bool dumped = false;
    if (REXCVAR_GET(tw2de_frame_dump) && refresh && !dumped && now - start > std::chrono::seconds(40)) {
      dumped = true;
      renut::gpu::render_thread::Queue([] { tw2de::renut_gpu::StartFrameDumpNow(); });
    }
  }
  g_last_start = now;
  __imp__sub_82EEAF48(ctx, base);
  g_last_end = std::chrono::steady_clock::now();
  g_last_took = std::chrono::duration<double, std::milli>(g_last_end - g_last_start).count();
}

// TW2 (2026-11-12): the render thread's idle loop. sub_82EEC008 polls its command queue without waiting: each turn
// asks sub_82585448(queue) whether it has a command (its count at +0xC above 0: 1), reads the clock and checks its refresh and loading-screen
// timers, and goes round again: a whole core (core 0, where the layout puts the thread alone) spent polling (62 % in
// the CPU profile of the intro, half of it in the clock). Here a 0 (nothing queued) at that call (return address
// 0x82EEC088) sleeps tw2de_render_idle_sleep_us first; the queue's producer is not woken any later, the render
// thread only sees a new frame that much later (at most), and the core is free for the other guest threads
// (tw2de_core_layout_others_mask). 0: polls as the game does.
// TW2 (2026-11-12, stock clocks): 200 by default (with others mask 7: prologue 16-21 -> 18-22 fps; the main menu's and
// the movies' audio no longer starved).
REXCVAR_DEFINE_INT32(tw2de_render_idle_sleep_us, 200, "Game/Enhancements",
                     "The render thread's idle poll waits this long when its queue is empty (0: the game's busy "
                     "poll)")
    .range(0, 5000);

REX_EXTERN(__imp__sub_82585448);
extern "C" REX_FUNC(sub_82585448) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82585448(ctx, base);
  if (lr == 0x82EEC088u && ctx.r3.u32 == 0) {
    const int32_t us = REXCVAR_GET(tw2de_render_idle_sleep_us);
    if (us > 0)
      std::this_thread::sleep_for(std::chrono::microseconds(us));
  }
}

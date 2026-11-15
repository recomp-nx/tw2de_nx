// tw2de (2026-11-09, tw2de_fix_load_fade): the frames shown after a loaded save's chapter screen, drawn from an
// unplaced camera (the field's ground black below the horizon, the party missing) while the fade-in runs.
//
// A save is loaded at its save point, inside the save point's event (zone state STATE_EVENT_ON_FIELD_IDLE, cinema
// com_ucon_ev), with the chapter screen over it. When the screen closes, two threads finish the event: the script ends
// it (STATE_EVENT_ON_FIELD_UNINIT, then its "ModeChange" callback and STATE_FIELD_IDLE) and the main thread creates the
// field's camera (createCinema <DistanceCinema>) and draws the field, while the fade-in (StartFade 4, 1 s from black)
// runs. In their order of the clean loads (the game's log, 2026-11-09) the camera is created after the mode change.
// Created first (in about a third of the loads here), the frames until the mode change are drawn from the new camera
// before it is placed: one frame most of the time (near black, early in the fade), 16 to 52 frames when the mode change
// came 0.5-1.7 s later (seen in two loads, the half-black field fading in). This shows black instead of those frames:
// after a save's load (main state STATE_LOAD_END), a DistanceCinema created during the event's end holds the present at
// black until the zone reaches STATE_FIELD_IDLE (3 s at most); the game runs as before.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_BOOL(tw2de_fix_load_fade, true, "Game/Fixes",
                    "After a loaded save's chapter screen, the frames the game draws from the field's camera before "
                    "the save point event has ended (the ground black, for up to 1.7 s of the fade-in) are shown "
                    "black")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
constexpr uint32_t kMainStateNames = 0x82A5E370;  // the main states' names, by index (sub_82463578's log)
constexpr uint32_t kZoneStateNames = 0x82A5B500;  // the zone states' names (sub_82448EC8's log)

uint32_t Load32(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
// A state's name from its table (empty for an index out of reason).
const char *StateName(const uint8_t *base, uint32_t table, uint32_t index) {
  if (index > 256)
    return "";
  const uint32_t p = Load32(base, table + index * 4);
  return p ? reinterpret_cast<const char *>(base + p) : "";
}

std::mutex g_mutex;
bool g_armed = false;  // a save's load until its event's end
bool g_event_end = false;  // the zone in STATE_EVENT_ON_FIELD_IDLE or STATE_EVENT_ON_FIELD_UNINIT
std::atomic<bool> g_hold{false};
std::chrono::steady_clock::time_point g_hold_since;
std::atomic<uint32_t> g_held{0};  // frames shown black
}  // namespace

// The Swap's question (renut_engine/gpu/d3d/hooks/device.cpp): this frame shown black.
bool FnxfXiiiLrLoadFadeHold() {
  if (!g_hold.load(std::memory_order_relaxed))
    return false;
  std::lock_guard lock(g_mutex);
  if (!g_hold)
    return false;
  if (std::chrono::steady_clock::now() - g_hold_since > std::chrono::seconds(3)) {
    g_hold = false;
    g_armed = false;
    REXLOG_INFO("[load fade] 3 s without the save point event's end: shown again ({} frame(s) black)",
                g_held.exchange(0));
    return false;
  }
  ++g_held;
  return true;
}

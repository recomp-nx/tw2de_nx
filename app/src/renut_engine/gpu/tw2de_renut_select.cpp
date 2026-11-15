// tw2de: the boot-time choice between tw2de's native renderer and Renut's (tw2de_renut_select.h).
#include "renut_engine/gpu/tw2de_renut_select.h"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(tw2de_diag_hook_time, false, "Game/Diagnostics",
                    "tw2de diagnostic (2026-10-28): the game threads' time in Renut's Direct3D hooks, per hook, "
                    "every 10 s ([hooktime])")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(tw2de_renderer, "native", "Game",
                      "Renderer, chosen at boot. native = tw2de's own native renderer (the default; the exact "
                      "reference, with every tw2de_* GPU and ring key). renut = Renut's Direct3D-level renderer "
                      "(github.com/masterspike52/reNut): N&B's Direct3D runs natively through plume, with no PM4 and no "
                      "EDRAM tile model; its own renut_* keys apply")
    .allowed({"native", "renut"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de::renut_gpu {

// tw2de (2026-10-19, CPU proposal 22): Active() is inline (tw2de_renut_select.h), a byte load after the first call.
namespace detail {
int8_t g_active = 0;
bool LatchActive() {
#if TW2DE_SWITCH_NO_VULKAN
  // tw2de (2026-10-20): a Switch build without the Vulkan driver has only Renut's renderer on deko3d (the native
  // renderer and the emulated GPU need Vulkan).
  static const bool active = [] {
    if (REXCVAR_GET(tw2de_renderer) != "renut")
      if (REXCVAR_GET(log_diagnostics)) REXLOG_WARN("tw2de_renderer = \"{}\" needs Vulkan, which this build does not have: Renut's renderer on deko3d "
                  "instead", REXCVAR_GET(tw2de_renderer));
    return true;
  }();
#else
  static const bool active = REXCVAR_GET(tw2de_renderer) == "renut";
#endif
  g_active = active ? 1 : -1;
  return active;
}

// tw2de (2026-10-28): tw2de_diag_hook_time.
int8_t g_hook_time = 0;
thread_local uint32_t t_hook_depth = 0;
bool LatchHookTime() {
  static const bool on = REXCVAR_GET(tw2de_diag_hook_time);
  g_hook_time = on ? 1 : -1;
  return on;
}
}  // namespace detail

namespace {
std::atomic<HookStat *> g_hook_stats{nullptr};
std::atomic<uint64_t> g_outer_ticks{0}, g_outer_calls{0};
}  // namespace

HookStat::HookStat(const char *n) : name(n) {
  HookStat *head = g_hook_stats.load(std::memory_order_relaxed);
  do {
    next = head;
  } while (!g_hook_stats.compare_exchange_weak(head, this, std::memory_order_release, std::memory_order_relaxed));
}

void HookTimer::NoteOuterHookTicks(uint64_t ticks) {
  g_outer_ticks.fetch_add(ticks, std::memory_order_relaxed);
  g_outer_calls.fetch_add(1, std::memory_order_relaxed);
}

void HookTimeReport(uint64_t frames) {
  if (!HookTimeOn())
    return;
  static uint64_t last_frames = 0, last_tick = 0;
  const uint64_t now = HookTicks();
  constexpr double kTicksPerMs = 19200.0;  // the Switch's system counter: 19.2 MHz
  if (!last_tick) {
    last_tick = now;
    last_frames = frames;
    return;
  }
  if (double(now - last_tick) < 10000.0 * kTicksPerMs || frames == last_frames)
    return;
  const double n = double(frames - last_frames);
  const double seconds = double(now - last_tick) / kTicksPerMs / 1000.0;
  last_tick = now;
  last_frames = frames;
  struct Row {
    const char *name;
    uint64_t ticks, calls;
  };
  std::vector<Row> rows;
  for (HookStat *s = g_hook_stats.load(std::memory_order_acquire); s; s = s->next) {
    const uint64_t t = s->ticks.exchange(0, std::memory_order_relaxed), c = s->calls.exchange(0, std::memory_order_relaxed);
    if (c)
      rows.push_back({s->name, t, c});
  }
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.ticks > b.ticks; });
  const uint64_t outer = g_outer_ticks.exchange(0), outer_calls = g_outer_calls.exchange(0);
  std::string top;
  for (size_t i = 0; i < rows.size() && i < 16; ++i)
    top += fmt::format("\n  {:<44} {:7.3f} ms a frame {:8.1f} calls a frame {:6.2f} us a call", rows[i].name,
                       double(rows[i].ticks) / kTicksPerMs / n, double(rows[i].calls) / n,
                       double(rows[i].ticks) / kTicksPerMs * 1000.0 / double(rows[i].calls));
  REXLOG_INFO("[hooktime] {:.1f} s, {:.0f} frames ({:.2f} fps): the game's threads in Renut's Direct3D hooks {:.3f} ms a "
              "frame ({:.1f} calls a frame, the outermost only){}",
              seconds, n, n / seconds, double(outer) / kTicksPerMs / n, double(outer_calls) / n, top);
}

}  // namespace tw2de::renut_gpu

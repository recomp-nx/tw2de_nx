/**
 * @file    gpu/render/dynamic_resolution.cpp
 * @brief   tw2de (2026-11-07): dynamic resolution of the scene (dynamic_resolution.h).
 */
#include "renut_engine/gpu/render/dynamic_resolution.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "renut_engine/display_sync.h"
#include "renut_engine/frame_interval.h"
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/render/render_scale.h"

REXCVAR_DEFINE_INT32(tw2de_drs, 0, "Game/Enhancements",
                     "Dynamic resolution (2026-11-07): the scene drawn at a lower resolution while the GPU would not "
                     "make the frame interval's frame rate (with tw2de_drs_margin to spare), down to "
                     "tw2de_drs_min percent of the resolution chosen (its height), in steps of 18 lines; 0 off")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_drs_min, 70, "Game/Enhancements",
                     "Dynamic resolution's lowest height, in percent of the resolution chosen")
    .range(50, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// TW2 (2026-11-14): 8 (was 20): the GPU's frame kept under 30.9 ms of frame interval 2's 33.3. TW2's
// Tutorial scene (32.9 ms at the game's own resolution) settles at 648 of 720 lines (a 1152x605 scene), 28.8 ms,
// 30.00 fps; with 20% to spare it would be held under 27.8 ms (612 lines).
REXCVAR_DEFINE_INT32(tw2de_drs_margin, 8, "Game/Enhancements",
                     "Dynamic resolution's margin (1-30 %, xc1fsr's dynres_margin): the GPU's frame time is kept at "
                     "least this much under the frame interval's; over it the resolution drops at once (to about the "
                     "frame time plus twice the margin), it rises when the time predicted for a higher one stays "
                     "under it")
    .range(1, 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_diag_drs_fixed, 0, "Game/Diagnostics",
                     "Test (2026-11-07): dynamic resolution's machinery at a fixed height, in percent of the "
                     "resolution chosen (50-100, rounded to its steps); 0 off (tw2de_drs decides)")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// TW2 (2026-11-14): see render_scale.h kDrsFirst.
REXCVAR_DEFINE_BOOL(tw2de_drs_sized, true, "Game/Enhancements",
                    "Dynamic resolution's levels as target sizes (2026-11-14; a 720-line scene): the scene's targets "
                    "and what resolves from them are made at the level's size and drawn whole. Off: the way of "
                    "2026-11-07 (targets of the full size drawn in part, the shaders scaling their texture "
                    "coordinates)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// TW2 (2026-11-14): the frame's GPU time against the picture's height, measured in one run.
REXCVAR_DEFINE_INT32(tw2de_diag_drs_sweep, 0, "Game/Diagnostics",
                     "Test (2026-11-14): dynamic resolution's levels one after another from the full height down to "
                     "50%, each for this many seconds of 3D gameplay, its GPU time a frame logged ([drs] sweep); then "
                     "the full height. 0 off")
    .range(0, 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// TW2 (2026-11-15, the lines at the screen's edge at a level change).
REXCVAR_DEFINE_INT32(tw2de_diag_drs_dump, 0, "Game/Diagnostics",
                     "Test (2026-11-15, with tw2de_frame_dump): the frame of dynamic resolution's Nth level change "
                     "and the one after it are dumped, and so are those of the changes that follow while no dump "
                     "runs (twelve dumps at most). 0 off")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de {
uint32_t RenderHeight();  // cpu/tw2de_render_resolution.cpp
namespace renut_gpu {
void RequestFrameDump();  // renut_engine/gpu/tw2de_renut_tuning.h
}
}

namespace renut::gpu {

void BumpSurfaceEpochLocked();  // surfaces.h

float g_drs_factor = 1.0f;

namespace {

// The levels: heights of 9 m lines (16 m x 9 m pixels at 16:9, whole numbers at every level), m from the scene's
// M = height / 9 (64, 80, 100, 120) down in steps of kStep (18 lines: the half-size buffers whole too).
constexpr int kStep = 2;
constexpr int kMaxLevels = 62;  // codes 1-63 (dynamic_resolution.h)
constexpr int kSettle = 4;           // samples left out after a change (the GPU time arrives a frame or two late)
constexpr int kEarlyN = 4;           // samples enough for an early drop
constexpr float kEarly = 1.15f;      // an early drop: the load this much over the limit
constexpr float kUpMargin = 4.0f;    // a rise: the predicted load this many points under the limit
constexpr float kLighter = 0.9f;     // the scene got lighter: the loads remembered are forgotten
using Clock = std::chrono::steady_clock;
constexpr auto kHold = std::chrono::milliseconds(500);
constexpr auto kMemory = std::chrono::seconds(3);

// tw2de (2026-11-08): geometry draws only now (DrsNoteSceneDraw); 20 was for all draws into the scene's targets with
// depth. Real-time cutscenes' close-ups have 7-19 geometry draws (frames switched to full resolution and back), the
// pause screen and menus none.
constexpr uint32_t kDrsSceneDraws = 4;  // a frame of 3D gameplay draws ~50-140 geometry draws (the field, save 97)

struct State {
  int fixed = 0;      // tw2de_diag_drs_fixed
  int sweep = 0;      // tw2de_diag_drs_sweep
  // tw2de (2026-11-08): this frame's scene draws (DrsNoteSceneDraw), and whether the frame is 3D gameplay (the last
  // frame had enough of them): the frames the GPU times arrive for later, kept by their order.
  uint32_t scene_draws = 0;
  uint32_t other_draws = 0;  // tw2de (2026-11-08): the other draws into the scene's targets with depth (the log's)
  uint32_t last_scene = 0, last_other = 0;  // the last frame's (the 10 s report's)
  bool gameplay = false;
  std::atomic<uint32_t> gameplay_bits{0};  // the last 32 frames, bit 0 the newest
  uint32_t gameplay_frames = 0;
  int big_m = 0;      // M (0: not known yet)
  int min_m = 0;
  int levels = 0;     // the lowest level's index
  std::atomic<int> level{0};
  // OnGpuFrame's (the render thread's)
  int held = 0, n = 0;
  float sum = 0.0f;
  Clock::time_point held_since{};
  float seen[kMaxLevels + 1] = {};
  Clock::time_point seen_at[kMaxLevels + 1] = {};
  int mem_level = -1;
  float mem_load = 0.0f;
  bool mem_arm = false;
  // the log's
  int applied = -1;
  float last_avg = 0.0f;
  uint32_t changes = 0;
  Clock::time_point report_at{};
  double drawn_sum = 0.0;
  uint32_t drawn_n = 0;
};
State g;

int LevelM(int level) { return std::max(g.min_m, g.big_m - kStep * level); }

// The GPU's frame time the frame interval allows, in ms.
float BudgetMs() {
  const uint64_t period = tw2de::display_sync::RefreshPeriodNs();
  const double refresh_ms = period ? double(period) / 1e6 : 1000.0 / 60.0;
  const uint32_t vblanks = tw2de::frame_interval::GameVblanks();
  return float(refresh_ms * double(std::max(vblanks, 1u)));
}

bool Setup() {
  if (g.big_m)
    return true;
  const uint32_t h = tw2de::RenderHeight();
  if (h < 90)
    return false;
  g.big_m = int(h / 9);
  const int pct = g.sweep ? 50 : g.fixed ? std::clamp(g.fixed, 50, 100) : std::clamp(REXCVAR_GET(tw2de_drs_min), 50, 100);
  int min_m = (g.big_m * pct + 99) / 100;
  min_m += (g.big_m - min_m) % kStep;  // a level of the steps from M
  g.min_m = std::clamp(min_m, 1, g.big_m);
  g.levels = std::min((g.big_m - g.min_m + kStep - 1) / kStep, kMaxLevels);
  if (DrsSized())
    g.levels = std::min(g.levels, int(RenderScale::kDrsLevels) - 1);
  if (g.fixed)
    g.level.store(g.levels);
  REXLOG_INFO("[drs] dynamic resolution{}: the scene's {} lines down to {} ({} levels of {} lines), margin {}%{}",
              g.fixed ? " (fixed: tw2de_diag_drs_fixed)" : "", h, 9 * g.min_m, g.levels, 9 * kStep,
              REXCVAR_GET(tw2de_drs_margin), DrsSized() ? "; the levels as target sizes (tw2de_drs_sized)" : "");
  return true;
}

}  // namespace

bool DrsOn() {
  static const bool on = [] {
    g.sweep = REXCVAR_GET(tw2de_diag_drs_sweep);
    g.fixed = g.sweep ? 0 : REXCVAR_GET(tw2de_diag_drs_fixed);
    return IsDeko3D() && (REXCVAR_GET(tw2de_drs) != 0 || g.fixed != 0 || g.sweep != 0);
  }();
  return on;
}

bool DrsSized() {
  static const bool on = DrsOn() && REXCVAR_GET(tw2de_drs_sized) && tw2de::RenderHeight() == 720;
  return on;
}

RenderScale DrsSceneScale() {
  RenderScale s;
  s.code = RenderScale::kDrsFirst + uint32_t(std::clamp(g.applied, 0, int(RenderScale::kDrsLevels) - 1));
  return s;
}

uint32_t DrsCode() {
  if (!DrsOn() || !g.big_m || DrsSized())
    return 0;
  return 1u + uint32_t(std::clamp(g.applied, 0, g.levels));
}

float DrsStep() { return g.big_m ? float(kStep) / float(g.big_m) : 0.0f; }

float DrsFillOfCode(uint32_t code) {
  if (!code || !g.big_m)
    return 1.0f;
  return float(LevelM(int(code) - 1)) / float(g.big_m);
}

uint32_t DrsDrawn(uint32_t host) {
  return std::max<uint32_t>(uint32_t(std::lround(double(host) * double(g_drs_factor))), 1u);
}

void DrsNoteSceneDraw(bool geometry) {
  if (geometry)
    ++g.scene_draws;
  else
    ++g.other_draws;
}

void DrsFrameStartLocked() {
  // tw2de (2026-11-08): only 3D gameplay is drawn lower (dynamic_resolution.h DrsNoteSceneDraw).
  const bool was = g.gameplay;
  g.gameplay = g.scene_draws >= kDrsSceneDraws;
  const uint32_t scene_draws = g.scene_draws, other_draws = g.other_draws;
  g.scene_draws = 0;
  g.other_draws = 0;
  g.last_scene = scene_draws;
  g.last_other = other_draws;
  g.gameplay_bits.store((g.gameplay_bits.load(std::memory_order_relaxed) << 1) | (g.gameplay ? 1u : 0u),
                        std::memory_order_relaxed);
  if (!DrsOn() || !Setup()) {
    g_drs_factor = 1.0f;
    return;
  }
  if (was != g.gameplay && g.changes < 400)
    REXLOG_INFO("[drs] {} ({} geometry draws, {} others)",
                g.gameplay ? "3D gameplay: dynamic resolution on" : "not 3D gameplay: full resolution", scene_draws,
                other_draws);
  const int level = g.gameplay ? std::clamp(g.level.load(std::memory_order_relaxed), 0, g.levels) : 0;
  const int m = LevelM(level);
  const bool sized = DrsSized();
  g_drs_factor = sized ? 1.0f : float(m) / float(g.big_m);
  if (level != g.applied) {
    if (sized)
      BumpSurfaceEpochLocked();  // the scene's targets are other ones from this frame on (DrsSceneScale)
    if (g.applied >= 0 && was && g.gameplay && g.changes++ < 400)  // the controller's (not gameplay's on / off)
      REXLOG_INFO("[drs] {} -> {} lines (GPU load {:.0f}% of {:.1f} ms)", 9 * LevelM(g.applied), 9 * m, g.last_avg,
                  BudgetMs());
    // tw2de_diag_drs_dump: this frame (the first of the new level) and the next.
    static const int dump_from = REXCVAR_GET(tw2de_diag_drs_dump);
    static int seen = 0, asked = 0;
    if (dump_from > 0 && g.applied >= 0 && was && g.gameplay && ++seen >= dump_from && asked < 12) {
      ++asked;
      REXLOG_INFO("[drs] frame dump asked for change {}: {} -> {} lines", seen, 9 * LevelM(g.applied), 9 * m);
      tw2de::renut_gpu::RequestFrameDump();
    }
    g.applied = level;
  }
  g.drawn_sum += 9.0 * m;
  ++g.drawn_n;
  const auto now = Clock::now();
  if (now - g.report_at >= std::chrono::seconds(10)) {
    if (g.report_at != Clock::time_point{} && g.drawn_n)
      REXLOG_INFO("[drs] last 10 s: {:.0f} lines on average, now {} ({} changes so far; last frame {} geometry draws, {} "
                  "others)",
                  g.drawn_sum / g.drawn_n, 9 * m, g.changes, g.last_scene, g.last_other);
    g.report_at = now;
    g.drawn_sum = 0.0;
    g.drawn_n = 0;
  }
}

// xc1fsr's dynamic resolution (source/program/fsr.cpp GpuCostUpdate, the GPU load rule), on the GPU's frame time.
void DrsOnGpuFrame(double ms) {
  if (!DrsOn() || g.fixed || !g.big_m || ms <= 0.0)
    return;
  const auto now = Clock::now();
  // tw2de (2026-11-08): frames that are not 3D gameplay (and the two after a change of it: the time arrives a frame
  // or two late) neither count nor move the level; the hold starts again.
  if ((g.gameplay_bits.load(std::memory_order_relaxed) & 7u) != 7u) {
    g.held = 0;
    g.n = 0;
    g.sum = 0.0f;
    g.held_since = now;
    return;
  }
  if (g.sweep) {
    // tw2de_diag_drs_sweep: the level's frames (but the first 8 after the change: the time arrives late, the targets
    // of the new size are made), then the next level.
    static Clock::time_point since{};
    static double sum = 0.0;
    static uint32_t n = 0;
    static int skip = 8;
    static bool done = false;
    if (done)
      return;
    if (skip > 0) {
      --skip;
      since = now;
      return;
    }
    sum += ms;
    ++n;
    if (now - since >= std::chrono::seconds(g.sweep)) {
      const int at = g.level.load(std::memory_order_relaxed);
      REXLOG_INFO("[drs] sweep: {} lines: GPU {:.2f} ms a frame ({} frames)", 9 * LevelM(at), sum / double(n), n);
      sum = 0.0;
      n = 0;
      skip = 8;
      done = at >= g.levels;
      g.level.store(done ? 0 : at + 1, std::memory_order_relaxed);
    }
    return;
  }
  const float load = float(ms) / BudgetMs() * 100.0f;
  const float margin = 1.0f + 0.01f * float(std::clamp(REXCVAR_GET(tw2de_drs_margin), 1, 30));
  const float limit = 100.0f / margin;
  int level = g.level.load(std::memory_order_relaxed);
  if (g.held_since == Clock::time_point{})
    g.held_since = now;
  if (++g.held > kSettle) {
    g.sum += load;
    ++g.n;
  }
  if (!g.n)
    return;
  const float avg = g.sum / float(g.n);
  const bool early = level < g.levels && g.n >= kEarlyN && avg > limit * kEarly;
  if (now - g.held_since < kHold && !early)
    return;
  g.last_avg = avg;
  g.seen[level] = avg;
  g.seen_at[level] = now;
  auto forget = [] {
    for (auto &a : g.seen_at)
      a = Clock::time_point{};
  };
  if (g.mem_arm) {
    g.mem_arm = false;
    g.mem_level = level;
    g.mem_load = avg;
  } else if (g.mem_level >= 0) {
    // The load at the remembered level, scaled to this one, is lighter now: the scene changed, the loads seen are old.
    const float hm = float(LevelM(g.mem_level)), ha = float(LevelM(level));
    if (avg * (hm * hm) / (ha * ha) < g.mem_load * kLighter) {
      forget();
      g.mem_level = -1;
    }
  }
  auto fresh = [&](int l) { return g.seen_at[l] != Clock::time_point{} && now - g.seen_at[l] < kMemory; };
  int next = level;
  const float hc = float(LevelM(level));
  if (avg > limit && level < g.levels) {
    // Down at once, to where the load is predicted at about the frame time plus twice the margin.
    const float aim = 100.0f / (2.0f * margin - 1.0f);
    next = level + 1;
    while (next < g.levels) {
      const float hn = float(LevelM(next));
      if (avg * (hn * hn) / (hc * hc) <= aim)
        break;
      ++next;
    }
  } else if (level > 0) {
    // Up while the load predicted (or seen lately, when higher) stays a few points under the limit.
    for (int u = level - 1; u >= 0; --u) {
      const float hu = float(LevelM(u));
      float pred = avg * (hu * hu) / (hc * hc);
      if (fresh(u) && g.seen[u] > pred)
        pred = g.seen[u];
      if (pred < limit - kUpMargin)
        next = u;
      else
        break;
    }
  }
  if (next > level)
    g.mem_arm = true;
  g.level.store(next, std::memory_order_relaxed);
  g.held = 0;
  g.n = 0;
  g.sum = 0.0f;
  g.held_since = now;
}

}  // namespace renut::gpu

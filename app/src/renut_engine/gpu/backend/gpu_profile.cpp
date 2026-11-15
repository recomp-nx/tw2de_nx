/**
 * @file    gpu/backend/gpu_profile.cpp
 */
#include "renut_engine/gpu/backend/gpu_profile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"     // NextToExecutable
#include "renut_engine/gpu/nx_shaders/shader_store.h"  // the dumped shaders' GLSL

REXCVAR_DEFINE_DOUBLE(renut_gpu_profile, 0.0, "Nuts&Bolts/Debug",
                      "Every this many seconds, log the GPU's time per frame by kind of work "
                      "(draws per target, resolves, mip levels, present). 0 = off.");
// tw2de (package 28b): the passes split by pixel shader (render/draw.cpp, ShaderProfileLabel), more lines.
REXCVAR_DECLARE(bool, tw2de_renut_diag_gpu_shaders);
// tw2de (2026-10-23): with the per-shader breakdown, the GLSL of this many of the costliest pixel and vertex shaders
// is written to tw2de_gpu_shaders/ next to the NRO (once each), to look at what makes them expensive. 0 = none.
REXCVAR_DEFINE_INT32(tw2de_renut_diag_gpu_shaders_dump, 0, "Game/Diagnostics",
                     "With tw2de_renut_diag_gpu_shaders: write the translated GLSL of this many of the costliest pixel "
                     "and vertex shaders to tw2de_gpu_shaders/ next to the NRO (each once). 0 = none")
    .range(0, 200);

namespace renut::gpu::gpu_profile {

bool g_recording_timed = false;

namespace {

// Timestamps one frame may write; past them its work stays with the last
// kind marked.
// tw2de (2026-10-27): 8192, not 2048 (a map load's frame of memexport draws, each marked: slow_frames below).
constexpr uint32_t kMaxMarks = 8192;
constexpr uint32_t kEndOfList = 0;  // a list's last timestamp: no span follows
constexpr uint32_t kOther = 1;      // a list's start, before any kind is marked

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  bool open = false;    // the slot's frame has begun recording
  bool active = false;  // ... and is profiled
  uint32_t count = 0;
  uint32_t list = 0;
  uint32_t current = kEndOfList;
  std::array<uint32_t, kMaxMarks> labels{};
  std::array<uint32_t, kMaxMarks> lists{};
  std::vector<uint32_t> draws;  // tw2de (2026-10-27): this frame's draws per label (the slow frame report)
  std::vector<uint64_t> amounts;  // tw2de (2026-10-27): and its amounts (AddAmount)
  std::array<uint64_t, kMaxMarks> span_amounts{};  // tw2de (2026-10-27): each span's own (the slow frame's spans)
};
std::array<Slot, kFramesInFlight> g_slots;

std::vector<std::string> g_names = {"(list end)", "other"};
std::unordered_map<std::string, uint32_t> g_ids = {{"(list end)", kEndOfList}, {"other", kOther}};

// Since the last report: GPU nanoseconds per kind, and frames.
std::vector<uint64_t> g_totals;
std::vector<uint64_t> g_draws;  // tw2de (package 28b): draws per kind (CountDraw)
// tw2de (2026-10-23): what each per-shader label stands for (SetLabelShaders); has = it is one.
struct LabelShaders {
  bool has = false;
  uint32_t target = 0;
  uint64_t vs = 0, ps = 0;
};
std::vector<LabelShaders> g_label_shaders;
std::unordered_map<uint64_t, bool> g_dumped;  // shader hashes whose GLSL was written (or could not be)
uint64_t g_frames = 0;
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();

void Write(VideoState &s, Slot &slot, uint32_t label) {
  s.command_list->writeTimestamp(slot.pool.get(), slot.count);
  slot.labels[slot.count] = label;
  slot.lists[slot.count] = slot.list;
  slot.span_amounts[slot.count] = 0;
  ++slot.count;
  slot.current = label;
}

// tw2de (2026-10-23): the per-shader labels summed by pixel shader and by vertex shader (a shader's time is that of
// the draws it took part in: a vertex-bound draw counts for its pixel shader too, and the other way round), the top of
// each in the log and every one of them in tw2de_gpu_shaders.txt next to the NRO (rewritten at each report). A label's
// time is from the timestamp before its first draw to the next one (the GPU writes a timestamp once the work before it
// is done), so overlapping draws are counted for the later one; with many draws a frame the sums are reliable, a
// single short draw's number less so.
struct ShaderSum {
  uint64_t ns = 0, draws = 0;
  uint32_t labels = 0;  // passes / pairings it appears in
  uint64_t other = 0;   // the costliest partner (the pixel shader's vertex shader or the other way round)
  uint64_t other_ns = 0;
};

void DumpGlsl(uint64_t hash, bool pixel) {
  if (!hash || g_dumped.count(hash))
    return;
  g_dumped[hash] = true;
  auto &store = renut::nxs::ShaderStore::Get();
  const renut::nxs::StoredShader *stored = store.IsOpen() ? store.Find(hash) : nullptr;
  std::string glsl;
  if (!stored || !store.ReadGlsl(*stored, glsl))
    return;
  std::error_code ec;
  const std::filesystem::path dir = tw2de::renut_gpu::NextToExecutable("tw2de_gpu_shaders");
  std::filesystem::create_directories(dir, ec);
  std::ofstream f(dir / fmt::format("{}_{:016X}.glsl", pixel ? "ps" : "vs", hash), std::ios::binary);
  f.write(glsl.data(), std::streamsize(glsl.size()));
}

void ShaderReport(uint64_t total, double per_frame) {
  std::unordered_map<uint64_t, ShaderSum> by_ps, by_vs;
  for (uint32_t i = 0; i < g_totals.size() && i < g_label_shaders.size(); ++i) {
    const LabelShaders &l = g_label_shaders[i];
    if (!l.has || !g_totals[i])
      continue;
    const uint64_t ns = g_totals[i], draws = i < g_draws.size() ? g_draws[i] : 0;
    auto add = [&](ShaderSum &sum, uint64_t other) {
      sum.ns += ns;
      sum.draws += draws;
      ++sum.labels;
      if (ns > sum.other_ns) {
        sum.other_ns = ns;
        sum.other = other;
      }
    };
    add(by_ps[l.ps], l.vs);
    add(by_vs[l.vs], l.ps);
  }
  auto ranked = [](const std::unordered_map<uint64_t, ShaderSum> &m) {
    std::vector<std::pair<uint64_t, ShaderSum>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second.ns > b.second.ns; });
    return v;
  };
  const auto ps = ranked(by_ps), vs = ranked(by_vs);
  const double frames = double(g_frames);
  auto line = [&](const std::pair<uint64_t, ShaderSum> &e, const char *kind, const char *other_kind) {
    const ShaderSum &sum = e.second;
    const double ms = double(sum.ns) / per_frame, draws = double(sum.draws) / frames;
    return fmt::format("\n  {:7.3f} ms {:5.1f}%  {:7.1f} draws  {:8.2f} us/draw  {} {:016X}  in {} pass(es), "
                       "mostly with {} {:016X}",
                       ms, total ? 100.0 * double(sum.ns) / double(total) : 0.0, draws,
                       sum.draws ? 1e-3 * double(sum.ns) / double(sum.draws) : 0.0, kind, e.first, sum.labels,
                       other_kind, sum.other);
  };
  std::string top;
  for (size_t i = 0; i < ps.size() && i < 25; ++i)
    top += line(ps[i], "PS", "VS");
  RGPU_INFO("GPU profile by pixel shader (a frame; the full list with vertex shaders: tw2de_gpu_shaders.txt):{}",
            top);

  // Everything, in a file next to the NRO.
  std::string text = fmt::format("GPU profile, {} timed frames, {:.3f} ms a frame\n\nBy pixel shader:", g_frames,
                                 double(total) / per_frame);
  for (const auto &e : ps)
    text += line(e, "PS", "VS");
  text += "\n\nBy vertex shader:";
  for (const auto &e : vs)
    text += line(e, "VS", "PS");
  text += "\n\nBy pass, vertex and pixel shader:";
  std::vector<std::pair<uint64_t, uint32_t>> labels;
  for (uint32_t i = 0; i < g_totals.size() && i < g_label_shaders.size(); ++i)
    if (g_label_shaders[i].has && g_totals[i])
      labels.emplace_back(g_totals[i], i);
  std::sort(labels.rbegin(), labels.rend());
  for (const auto &[ns, i] : labels) {
    const uint64_t draws = i < g_draws.size() ? g_draws[i] : 0;
    text += fmt::format("\n  {:7.3f} ms  {:7.1f} draws  {:8.2f} us/draw  {}", double(ns) / per_frame,
                        double(draws) / frames, draws ? 1e-3 * double(ns) / double(draws) : 0.0, g_names[i]);
  }
  text += "\n";
  std::ofstream(tw2de::renut_gpu::NextToExecutable("tw2de_gpu_shaders.txt"), std::ios::binary | std::ios::trunc)
      .write(text.data(), std::streamsize(text.size()));

  const int dump = REXCVAR_GET(tw2de_renut_diag_gpu_shaders_dump);
  for (int i = 0; i < dump && size_t(i) < ps.size(); ++i)
    DumpGlsl(ps[i].first, true);
  for (int i = 0; i < dump && size_t(i) < vs.size(); ++i)
    DumpGlsl(vs[i].first, false);
}

void Report(double interval) {
  const auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration<double>(now - g_last_report).count() < interval || !g_frames)
    return;
  g_last_report = now;
  std::vector<std::pair<uint64_t, uint32_t>> kinds;
  uint64_t total = 0;
  for (uint32_t i = 0; i < g_totals.size(); ++i) {
    if (g_totals[i]) {
      kinds.emplace_back(g_totals[i], i);
      total += g_totals[i];
    }
  }
  std::sort(kinds.rbegin(), kinds.rend());
  const double per_frame = 1e6 * double(g_frames);
  std::string lines;
  const size_t shown = REXCVAR_GET(tw2de_renut_diag_gpu_shaders) ? 60 : 24;
  for (size_t i = 0; i < kinds.size() && i < shown; ++i) {
    const uint32_t k = kinds[i].second;
    const uint64_t draws = k < g_draws.size() ? g_draws[k] : 0;
    lines += fmt::format("\n  {:7.3f} ms  {}", double(kinds[i].first) / per_frame, g_names[k]);
    if (draws)
      lines += fmt::format("  ({:.1f} draws a frame)", double(draws) / double(g_frames));
  }
  RGPU_INFO("GPU profile: {:.3f} ms per frame over {} frames{}", double(total) / per_frame,
            g_frames, lines);
  if (REXCVAR_GET(tw2de_renut_diag_gpu_shaders))
    ShaderReport(total, per_frame);
  std::fill(g_totals.begin(), g_totals.end(), 0);
  std::fill(g_draws.begin(), g_draws.end(), 0);
  g_frames = 0;
}

// tw2de (2026-10-27): a frame that kept the GPU busy for 150 ms or more (not a busy fight's slow frames: 15 FPS is
// 67 ms), logged on its own (the map load where a
// frame took ~500 ms of GPU time and a kickoff timed out, deko3d.log): its labels by GPU time, with their draws. Up to
// 24 such frames a run.
void SlowFrame(const Slot &slot, const uint64_t *ns, const std::vector<uint64_t> &frame, uint64_t total) {
  static uint32_t logged = 0;
  if (total < 150000000ull || logged >= 24)
    return;
  ++logged;
  std::vector<std::pair<uint64_t, uint32_t>> kinds;
  for (uint32_t i = 0; i < frame.size(); ++i)
    if (frame[i])
      kinds.emplace_back(frame[i], i);
  std::sort(kinds.rbegin(), kinds.rend());
  uint64_t draws = 0;
  for (uint32_t d : slot.draws)
    draws += d;
  std::string lines;
  for (size_t i = 0; i < kinds.size() && i < 16; ++i) {
    const uint32_t k = kinds[i].second;
    lines += fmt::format("\n  {:8.3f} ms  {} draws  {}", double(kinds[i].first) / 1e6,
                         k < slot.draws.size() ? slot.draws[k] : 0u, g_names[k]);
    if (k < slot.amounts.size() && slot.amounts[k])
      lines += fmt::format("  [{}]", slot.amounts[k]);
    // tw2de (2026-10-27): the shaders of the 4 costliest labels that name them (memexport draws: draw.cpp), their
    // GLSL in tw2de_gpu_shaders/ next to the NRO (each once).
    if (i < 4 && k < g_label_shaders.size() && g_label_shaders[k].has) {
      DumpGlsl(g_label_shaders[k].vs, false);
      DumpGlsl(g_label_shaders[k].ps, true);
    }
  }
  RGPU_WARN("GPU slow frame: {:.3f} ms of GPU work in {} command lists, {} draws, {} timestamps{}:{}",
            double(total) / 1e6, slot.list, draws, slot.count,
            slot.count + 2 >= kMaxMarks ? " (all used: the rest is counted under the last label)" : "", lines);
  // tw2de (2026-10-27): the frame span by span from its first memexport label on (60 spans, the first 6 such
  // frames): which work each draw's time goes to, and its amount, to tell a cost per draw from one per vertex.
  static uint32_t spans_logged = 0;
  if (spans_logged >= 6)
    return;
  ++spans_logged;
  uint32_t first = slot.count;
  for (uint32_t i = 0; i < slot.count; ++i) {
    if (g_names[slot.labels[i]].rfind("memexport", 0) == 0 || g_names[slot.labels[i]] == "exported stream conversion") {
      first = i;
      break;
    }
  }
  std::string spans;
  for (uint32_t i = first, n = 0; i + 1 < slot.count && n < 60; ++i) {
    if (slot.labels[i] == kEndOfList || slot.lists[i] != slot.lists[i + 1])
      continue;
    spans += fmt::format("\n  {:8.1f} us  {:>10}  {}", double(ns[i + 1] > ns[i] ? ns[i + 1] - ns[i] : 0) / 1e3,
                         slot.span_amounts[i], g_names[slot.labels[i]]);
    ++n;
  }
  RGPU_WARN("GPU slow frame, span by span (time, amount, work):{}", spans);
}

} // namespace

uint32_t Label(std::string_view name) {
  auto it = g_ids.find(std::string(name));
  if (it != g_ids.end())
    return it->second;
  const uint32_t id = uint32_t(g_names.size());
  g_names.emplace_back(name);
  g_ids.emplace(std::string(name), id);
  return id;
}

std::string_view Name(uint32_t label) {
  return label < g_names.size() ? std::string_view(g_names[label]) : std::string_view("?");
}

void SetLabelShaders(uint32_t label, uint32_t target_label, uint64_t vs_hash, uint64_t ps_hash) {
  if (g_label_shaders.size() <= label)
    g_label_shaders.resize(label + 1);
  g_label_shaders[label] = {true, target_label, vs_hash, ps_hash};
}

void CountDraw(VideoState &s, uint32_t label) {
  // Counted when recorded; reported with the frames the GPU profile collected (both every frame).
  Slot &slot = g_slots[s.frame];
  if (!slot.active)
    return;
  if (slot.draws.size() <= label)
    slot.draws.resize(label + 1);
  ++slot.draws[label];
  if (g_draws.size() <= label)
    g_draws.resize(label + 1);
  ++g_draws[label];
}

void AddAmount(VideoState &s, uint32_t label, uint64_t amount) {
  Slot &slot = g_slots[s.frame];
  if (!slot.active)
    return;
  if (slot.amounts.size() <= label)
    slot.amounts.resize(label + 1);
  slot.amounts[label] += amount;
  if (slot.count)
    slot.span_amounts[slot.count - 1] += amount;  // the span open now (its timestamp is the last written)
}

void MarkAlways(VideoState &s, uint32_t label) {
  Slot &slot = g_slots[s.frame];
  if (!slot.active || !s.command_list || slot.count + 2 >= kMaxMarks)
    return;
  Write(s, slot, label);
}

void Mark(VideoState &s, uint32_t label) {
  Slot &slot = g_slots[s.frame];
  if (!slot.active || !s.command_list || label == slot.current || slot.count + 2 >= kMaxMarks)
    return;
  Write(s, slot, label);
}

void BeginList(VideoState &s) {
  Slot &slot = g_slots[s.frame];
  if (!slot.open) {
    slot.open = true;
    slot.count = 0;
    slot.list = 0;
    std::fill(slot.draws.begin(), slot.draws.end(), 0u);
    std::fill(slot.amounts.begin(), slot.amounts.end(), 0ull);
    slot.active = REXCVAR_GET(renut_gpu_profile) > 0.0 && s.device->getCapabilities().queryPools;
    // tw2de (2026-10-19, GPU): with the per-shader breakdown (a timestamp at nearly every draw), one frame in
    // kShaderSample is timed; the report's averages are over the timed frames (draws are counted in them only).
    static uint32_t frames_seen = 0;
    constexpr uint32_t kShaderSample = 4;
    if (slot.active && REXCVAR_GET(tw2de_renut_diag_gpu_shaders) && (frames_seen++ % kShaderSample) != 0)
      slot.active = false;
    if (slot.active && !slot.pool) {
      slot.pool = s.device->createQueryPool(kMaxMarks);
      slot.active = slot.pool != nullptr;
    }
    // Vulkan queries are reset before they are written again.
    if (slot.active)
      s.command_list->resetQueryPool(slot.pool.get(), 0, kMaxMarks);
  }
  g_recording_timed = slot.active;  // tw2de (2026-10-19, render-thread proposal 7)
  if (slot.active && slot.count + 2 < kMaxMarks)
    Write(s, slot, kOther);
}

void EndList(VideoState &s, bool last_in_frame) {
  Slot &slot = g_slots[s.frame];
  if (!slot.active || !s.command_list)
    return;
  if (slot.count < kMaxMarks)
    Write(s, slot, kEndOfList);
  ++slot.list;
  // Vulkan returns no results while any query of the pool is unwritten.
  // tw2de (package 28b): unless the pool reads only the written ones (plume's queryResults(count)).
  if (last_in_frame && IsVulkan() && !slot.pool->partialResults()) {
    for (uint32_t i = slot.count; i < kMaxMarks; ++i)
      s.command_list->writeTimestamp(slot.pool.get(), i);
  }
}

void Collect(VideoState &s, uint32_t index) {
  (void)s;
  Slot &slot = g_slots[index];
  const bool active = slot.active && slot.count > 1;
  slot.open = false;
  slot.active = false;
  if (!active)
    return;
  slot.pool->queryResults(slot.count);  // tw2de: the written ones only, where the backend can
  const uint64_t *ns = slot.pool->getResults();
  if (g_totals.size() < g_names.size())
    g_totals.resize(g_names.size());
  // tw2de (2026-10-27): this frame's own sums too, for the slow frame report.
  static std::vector<uint64_t> frame;
  frame.assign(g_names.size(), 0);
  uint64_t frame_total = 0;
  for (uint32_t i = 0; i + 1 < slot.count; ++i) {
    if (slot.labels[i] == kEndOfList || slot.lists[i] != slot.lists[i + 1] || ns[i + 1] <= ns[i])
      continue;
    g_totals[slot.labels[i]] += ns[i + 1] - ns[i];
    frame[slot.labels[i]] += ns[i + 1] - ns[i];
    frame_total += ns[i + 1] - ns[i];
  }
  SlowFrame(slot, ns, frame, frame_total);
  ++g_frames;
  Report(REXCVAR_GET(renut_gpu_profile));
}

} // namespace renut::gpu::gpu_profile

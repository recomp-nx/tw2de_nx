// tw2de (2026-11-01, tw2de_diag_move_floor): what moves the boat (zone z008), and whether its physics bodies
// are moved with it, for diagnostics.
//
// The boat is a layout ("Lay", Stella) instance object moved by cinema clips. The move / transform clips' update
// (sub_82948288, LayMovePosClip's: vtable 0x821637C0 slot 2 -> sub_82948880 -> here, f1 = t) interpolates nine
// values, start + delta * t, each with its own enable byte (clip data +60..+68: position x y z, rotation, scale),
// keeps them at clip +140..+172 and hands them to the target object (its vtable +52 position, +60 rotation, +68
// scale). A physics body's pose is set by sub_828A6F80 (position: pose entry +0) and sub_828A70C0 (rotation:
// +16, from a matrix through sub_82295E28).
// With the cvar, at most four lines a second for each:
//   [move_floor] clip  <clip> t <t> axes <enabled ones> pos (x y z) rot (x y z) scale (x y z) target <object>
//   [move_floor] moved / watched: every 5 s, the bodies that moved more than 1 (and, once one moved more than 3,
//                      where it is from then on, moving or not)
//   [move_floor] body  <body> pos (x y z) | rot matrix rows; inside a clip update or not (outside: only bodies set
//                      within 10 of the last position a clip gave); caller
// so that the boat's position as the clip sets it can be compared with its bodies' position.
// Off (the default): the three functions run as before, a cvar read more.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(tw2de_diag_move_floor, false, "Game/Diagnostics",
                    "Diagnostic: log the layout move / transform clips (the boat's motion) and the physics bodies' pose "
                    "updates")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tw2de (2026-11-02): a test, not a fix. Lightning is blocked on the ramp at the boat's edge (x ~93.7, y -2.19,
// 0.35 below the deck's -1.84). With this above 0, each time her position (pose entries 0 / 2) is set inside that
// spot (x 93.0..94.3, y -2.30..-2.05, z 16..22), it is set this much higher: if she then boards, the block is that
// step. A line (at most four a second) for each lift. 0 (the default): nothing changed.
REXCVAR_DEFINE_DOUBLE(tw2de_diag_lift_lightning, 0.0, "Game/Diagnostics",
                      "Diagnostic test: raise Lightning by this much where she is blocked stepping onto the z008 boat "
                      "(0 = off; try 0.4)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82948288);  // the move / transform clip's update (r3 clip, r4 clip data, f1 t)
REX_EXTERN(__imp__sub_828A6F80);  // body position (r3 body, r4 -> vector)
REX_EXTERN(__imp__sub_828A70C0);  // body rotation (r3 body, r4 -> matrix)
REX_EXTERN(__imp__sub_828A7200);  // body collision on / off (r3 body, r4 on): pose entry +102 bit 0x1000

namespace {
uint32_t Load32(const uint8_t *base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, base + ea, 4);
  return __builtin_bswap32(v);
}
float LoadF(const uint8_t *base, uint32_t ea) {
  const uint32_t v = Load32(base, ea);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// At most four lines a second for each kind; the calls in between counted.
struct Rate {
  std::atomic<int64_t> next{0};
  std::atomic<uint32_t> calls{0};
  bool Take(uint32_t &since) {
    since = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const int64_t now = NowMs();
    int64_t n = next.load(std::memory_order_relaxed);
    if (now < n || !next.compare_exchange_strong(n, now + 250, std::memory_order_relaxed))
      return false;
    calls.fetch_sub(since, std::memory_order_relaxed);
    return true;
  }
};
Rate g_clip, g_body_in, g_body_out, g_rot_in;

bool On() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(tw2de_diag_move_floor);
    if (v)
      REXLOG_WARN("[move_floor] on (tw2de_diag_move_floor)");
    return v;
  }();
  return on;
}

// Every 5 s: how many clip updates and body pose sets there were (so that a run with none of the lines below still
// shows the diagnostic was on and what ran).
std::atomic<uint64_t> g_n_clip{0}, g_n_pos{0}, g_n_pos_in{0}, g_n_rot{0};
std::atomic<int64_t> g_next_count{0};
void Counts() {
  const int64_t now = NowMs();
  int64_t n = g_next_count.load(std::memory_order_relaxed);
  if (now < n || !g_next_count.compare_exchange_strong(n, now + 5000, std::memory_order_relaxed))
    return;
  REXLOG_WARN("[move_floor] so far: {} clip updates, {} body positions set ({} inside clips), {} body rotations set",
              g_n_clip.load(), g_n_pos.load(), g_n_pos_in.load(), g_n_rot.load());
}

// The bodies that move: for each body set by sub_828A6F80, its position at the start of the 5 s window and now, and
// the path it went. Every 5 s the ones that moved more than 1 (net) are logged (the boat, the characters), and those
// that ever moved more than 3 in one window are watched: their position logged every 5 s from then on, moving or not
// (the boat at rest at the dock; where Lightning stands when she is stuck).
struct Track {
  uint32_t body = 0, lr = 0;
  float first[3] = {}, last[3] = {};
  double path = 0;
  bool watched = false;
};
constexpr size_t kTracks = 8192;
Track g_tracks[kTracks];
std::mutex g_tracks_mutex;
std::atomic<int64_t> g_next_movers{0};

void TrackBody(uint32_t body, const float p[3], uint32_t lr) {
  if (!body || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
    return;
  std::lock_guard lock(g_tracks_mutex);
  size_t i = (body * 0x9E3779B1u) & (kTracks - 1);
  for (size_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kTracks - 1)) {
    Track &t = g_tracks[i];
    if (t.body == body) {
      const float dx = p[0] - t.last[0], dy = p[1] - t.last[1], dz = p[2] - t.last[2];
      t.path += std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz);
      std::copy(p, p + 3, t.last);
      t.lr = lr;
      return;
    }
    if (t.body == 0) {
      t.body = body;
      t.lr = lr;
      std::copy(p, p + 3, t.first);
      std::copy(p, p + 3, t.last);
      return;
    }
  }
}

// The physics' pose pool itself: every body's pose is an entry of 0x70 bytes, *(*(body + 0xC)) + *(body + 8) * 0x70,
// position at +0 (sub_828A6F80 writes it, other setters too: the boat's is not set through sub_828A6F80). The pools
// the bodies set through sub_828A6F80 live in are noted (their base, the highest index seen), and every 5 s each is
// read whole, so that a body moved by any setter shows: entries that moved more than 1 since the last scan, and those
// that once moved more than 3 watched from then on (position, the flags at +100 and +102).
struct Pool {
  uint32_t base = 0, max_index = 0;
  std::vector<float> last;  // 3 per entry
  std::vector<uint8_t> watched;
};
Pool g_pools[4];
std::mutex g_pools_mutex;

bool Readable(const uint8_t *base, uint32_t ea, uint32_t length) {
  for (uint32_t page = ea & ~0xFFFu; page < ea + length; page += 0x1000) {
    size_t len = 0x1000;
    rex::memory::PageAccess access;
    if (!rex::memory::QueryProtect(const_cast<uint8_t *>(base + page), len, access) ||
        (static_cast<int>(access) & static_cast<int>(rex::memory::PageAccess::kReadOnly)) == 0)
      return false;
  }
  return true;
}

void NotePool(const uint8_t *base, uint32_t body) {
  if (!Readable(base, body + 8, 8))
    return;
  const uint32_t holder = Load32(base, body + 0xC), index = Load32(base, body + 8);
  if (!holder || index > 0x4000 || !Readable(base, holder, 4))
    return;
  const uint32_t pool_base = Load32(base, holder);
  if (!pool_base)
    return;
  std::lock_guard lock(g_pools_mutex);
  for (Pool &p : g_pools) {
    if (p.base == pool_base) {
      p.max_index = std::max(p.max_index, index);
      return;
    }
    if (p.base == 0) {
      p.base = pool_base;
      p.max_index = index;
      return;
    }
  }
}

// tw2de (2026-11-01): the dock of zone z008's boat (x 60..110, y -12..14, z 8..28, the pool's coordinates):
// every 10 s every entry there, moving or not (a static collision box on the walkway is in no moved/watched line).
bool AtDock(float x, float y, float z) {
  return x > 60.0f && x < 110.0f && y > -12.0f && y < 14.0f && z > 8.0f && z < 28.0f;
}
uint32_t g_scans = 0;

void ScanPools(const uint8_t *base) {
  std::lock_guard lock(g_pools_mutex);
  const bool dump = (g_scans++ % 2) == 1;
  for (size_t pi = 0; pi < 4; ++pi) {
    Pool &p = g_pools[pi];
    if (!p.base)
      continue;
    const uint32_t count = std::min<uint32_t>(p.max_index + 1024, 0x4000);  // static bodies may sit above the highest index set
    if (!Readable(base, p.base, count * 0x70)) {
      REXLOG_WARN("[move_floor] pool {} at {:08X}: {} entries not all readable", pi, p.base, count);
      continue;
    }
    const bool first = p.last.empty();
    p.last.resize(size_t(count) * 3, 0.0f);
    p.watched.resize(count, 0);
    uint32_t moved = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t e = p.base + i * 0x70;
      const float x = LoadF(base, e), y = LoadF(base, e + 4), z = LoadF(base, e + 8);
      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        continue;
      float *l = &p.last[size_t(i) * 3];
      const float dx = x - l[0], dy = y - l[1], dz = z - l[2];
      const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      const uint16_t f100 = uint16_t(Load32(base, e + 100) >> 16), f102 = uint16_t(Load32(base, e + 100));
      if (!first && d > 1.0f && moved < 24) {
        ++moved;
        REXLOG_WARN("[move_floor] pool {} entry {}: ({:.3f} {:.3f} {:.3f}) -> ({:.3f} {:.3f} {:.3f}), moved {:.3f} in 5 s; "
                    "flags {:04X} {:04X}",
                    pi, i, l[0], l[1], l[2], x, y, z, d, f100, f102);
        if (d > 3.0f)
          p.watched[i] = 1;
      } else if (p.watched[i]) {
        REXLOG_WARN("[move_floor] pool {} entry {} watched: ({:.3f} {:.3f} {:.3f}), moved {:.3f} in 5 s; flags {:04X} "
                    "{:04X}; rotation ({:.4f} {:.4f} {:.4f} {:.4f})",
                    pi, i, x, y, z, d, f100, f102, LoadF(base, e + 16), LoadF(base, e + 20), LoadF(base, e + 24),
                    LoadF(base, e + 28));
      }
      if (dump && AtDock(x, y, z) && !p.watched[i] && false)
        REXLOG_WARN("[move_floor] pool {} entry {} at the dock: ({:.3f} {:.3f} {:.3f}) rotation ({:.4f} {:.4f} {:.4f} "
                    "{:.4f}) +32 ({:.3f} {:.3f} {:.3f}) +48 ({:.3f} {:.3f} {:.3f}) +96 {:.3f} flags {:04X} {:04X}",
                    pi, i, x, y, z, LoadF(base, e + 16), LoadF(base, e + 20), LoadF(base, e + 24),
                    LoadF(base, e + 28), LoadF(base, e + 32), LoadF(base, e + 36), LoadF(base, e + 40),
                    LoadF(base, e + 48), LoadF(base, e + 52), LoadF(base, e + 56), LoadF(base, e + 96), f100, f102);
      l[0] = x;
      l[1] = y;
      l[2] = z;
    }
  }
}

// tw2de (2026-11-02): every 2 s every pool entry where Lightning is blocked (x 85..100, y -6..6, z 12..24),
// moving or not, collision on or off: what is there while she walks in place, and what is not when she can pass.
std::atomic<int64_t> g_next_near{0};
void DumpNear(const uint8_t *base) {
  const int64_t now = NowMs();
  int64_t n = g_next_near.load(std::memory_order_relaxed);
  if (now < n || !g_next_near.compare_exchange_strong(n, now + 2000, std::memory_order_relaxed))
    return;
  std::lock_guard lock(g_pools_mutex);
  for (size_t pi = 0; pi < 4; ++pi) {
    const Pool &p = g_pools[pi];
    if (!p.base)
      continue;
    const uint32_t count = std::min<uint32_t>(p.max_index + 1024, 0x4000);
    if (!Readable(base, p.base, count * 0x70))
      continue;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t e = p.base + i * 0x70;
      const float x = LoadF(base, e), y = LoadF(base, e + 4), z = LoadF(base, e + 8);
      if (!(x > 85.0f && x < 100.0f && y > -6.0f && y < 6.0f && z > 12.0f && z < 24.0f))
        continue;
      const uint32_t f = Load32(base, e + 100);
      REXLOG_WARN("[move_floor] near: pool {} entry {} ({:.3f} {:.3f} {:.3f}) q ({:.4f} {:.4f} {:.4f} {:.4f}) +32 ({:.3f} "
                  "{:.3f} {:.3f}) +48 ({:.3f} {:.3f} {:.3f}) +96 {:.3f} flags {:04X} {:04X} collision {}",
                  pi, i, x, y, z, LoadF(base, e + 16), LoadF(base, e + 20), LoadF(base, e + 24), LoadF(base, e + 28),
                  LoadF(base, e + 32), LoadF(base, e + 36), LoadF(base, e + 40), LoadF(base, e + 48),
                  LoadF(base, e + 52), LoadF(base, e + 56), LoadF(base, e + 96), f >> 16, f & 0xFFFF,
                  (f & 0x1000) ? "on" : "off");
    }
  }
}

void Movers(const uint8_t *base) {
  DumpNear(base);
  const int64_t now = NowMs();
  int64_t n = g_next_movers.load(std::memory_order_relaxed);
  if (now < n || !g_next_movers.compare_exchange_strong(n, now + 5000, std::memory_order_relaxed))
    return;
  std::vector<std::pair<float, size_t>> moved;
  std::vector<size_t> watched;
  std::lock_guard lock(g_tracks_mutex);
  for (size_t i = 0; i < kTracks; ++i) {
    Track &t = g_tracks[i];
    if (!t.body)
      continue;
    const float dx = t.last[0] - t.first[0], dy = t.last[1] - t.first[1], dz = t.last[2] - t.first[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d > 1.0f)
      moved.push_back({d, i});
    if (d > 3.0f && !t.watched && watched.size() < 16) {
      size_t count = 0;
      for (size_t k = 0; k < kTracks; ++k)
        count += g_tracks[k].watched;
      if (count < 16)
        t.watched = true;
    }
    if (t.watched)
      watched.push_back(i);
  }
  std::sort(moved.begin(), moved.end(), [](auto &a, auto &b) { return a.first > b.first; });
  for (size_t k = 0; k < moved.size() && k < 8; ++k) {
    const Track &t = g_tracks[moved[k].second];
    REXLOG_WARN("[move_floor] moved: body {:08X} ({:.3f} {:.3f} {:.3f}) -> ({:.3f} {:.3f} {:.3f}), net {:.3f}, path "
                "{:.3f} in 5 s; caller {:08X}",
                t.body, t.first[0], t.first[1], t.first[2], t.last[0], t.last[1], t.last[2], moved[k].first, t.path,
                t.lr);
  }
  for (size_t i : watched) {
    const Track &t = g_tracks[i];
    REXLOG_WARN("[move_floor] watched: body {:08X} at ({:.3f} {:.3f} {:.3f}), went {:.3f} in 5 s; caller {:08X}", t.body,
                t.last[0], t.last[1], t.last[2], t.path, t.lr);
  }
  for (size_t i = 0; i < kTracks; ++i) {
    Track &t = g_tracks[i];
    std::copy(t.last, t.last + 3, t.first);
    t.path = 0;
  }
  ScanPools(base);
}

thread_local int t_in_clip = 0;
thread_local uint32_t t_clip = 0;

// The last position a clip that moves position set (a body set outside clips is logged only within 10 of it:
// the boat's own bodies, not every character's).
std::atomic<uint32_t> g_last_pos[3] = {};
std::atomic<bool> g_have_pos{false};
float Bits(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
uint32_t Bits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
}  // namespace

extern "C" REX_FUNC(sub_82948288) {
  if (!On()) {
    __imp__sub_82948288(ctx, base);
    return;
  }
  const uint32_t clip = ctx.r3.u32, data = ctx.r4.u32;
  const double t = ctx.f1.f64;
  g_n_clip.fetch_add(1, std::memory_order_relaxed);
  Counts();
  ++t_in_clip;
  const uint32_t outer = t_clip;
  t_clip = clip;
  __imp__sub_82948288(ctx, base);
  t_clip = outer;
  --t_in_clip;
  if (!clip || !data)
    return;
  if (base[data + 60] || base[data + 61] || base[data + 62]) {
    for (int i = 0; i < 3; ++i)
      g_last_pos[i].store(Load32(base, clip + 140 + 4 * i), std::memory_order_relaxed);
    g_have_pos.store(true, std::memory_order_relaxed);
  }
  uint32_t since;
  if (!g_clip.Take(since))
    return;
  std::string axes;
  static const char *const kAxis[9] = {"px", "py", "pz", "rx", "ry", "rz", "sx", "sy", "sz"};
  for (int i = 0; i < 9; ++i)
    if (base[data + 60 + i])
      axes += std::string(axes.empty() ? "" : ",") + kAxis[i];
  float v[9];
  for (int i = 0; i < 9; ++i)
    v[i] = LoadF(base, clip + 140 + 4 * i);
  const uint32_t target = Load32(base, clip + 36);
  REXLOG_WARN("[move_floor] clip {:08X} t {:.4f} axes {} pos ({:.3f} {:.3f} {:.3f}) rot ({:.3f} {:.3f} {:.3f}) scale "
              "({:.3f} {:.3f} {:.3f}) target-holder {:08X}; {} updates since the last line",
              clip, t, axes.empty() ? "none" : axes, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], target,
              since);
}

namespace {
void Store32(uint8_t *base, uint32_t ea, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + ea, &v, 4);
}
// Raises Lightning's new position at the blocked spot by tw2de_diag_lift_lightning (see the cvar).
void LiftLightning(uint8_t *base, uint32_t body, uint32_t p, uint32_t lr) {
  static const float lift = static_cast<float>(REXCVAR_GET(tw2de_diag_lift_lightning));
  if (!(lift > 0.0f) || !p || !Readable(base, p, 12) || !Readable(base, body + 8, 4))
    return;
  const uint32_t index = Load32(base, body + 8);
  if (index != 0 && index != 2)
    return;
  const float x = LoadF(base, p), y = LoadF(base, p + 4), z = LoadF(base, p + 8);
  if (!(x > 93.0f && x < 94.3f && y > -2.30f && y < -2.05f && z > 16.0f && z < 22.0f))
    return;
  const float ny = y + lift;
  uint32_t bits;
  std::memcpy(&bits, &ny, 4);
  Store32(base, p + 4, bits);
  static Rate rate;
  static std::atomic<uint32_t> lifts{0};
  const uint32_t n = lifts.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t since;
  if (rate.Take(since))
    REXLOG_WARN("[lift] entry {} at ({:.4f} {:.4f} {:.4f}) set to y {:.4f}; caller {:08X}; {} lifts so far", index, x,
                y, z, ny, lr, n);
}
}  // namespace

extern "C" REX_FUNC(sub_828A6F80) {
  LiftLightning(base, ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  if (!On()) {
    __imp__sub_828A6F80(ctx, base);
    return;
  }
  const uint32_t body = ctx.r3.u32, p = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  g_n_pos.fetch_add(1, std::memory_order_relaxed);
  if (t_in_clip)
    g_n_pos_in.fetch_add(1, std::memory_order_relaxed);
  Counts();
  __imp__sub_828A6F80(ctx, base);
  if (!p)
    return;
  {
    const float pos[3] = {LoadF(base, p), LoadF(base, p + 4), LoadF(base, p + 8)};
    TrackBody(body, pos, lr);
    NotePool(base, body);
    // tw2de (2026-11-02): Lightning (pose entries 0 and 2 in z008) frame by frame where she steps from the
    // walkway onto the boat: when blocked she rests 0.35 below the deck's surface (y -2.19 against -1.84).
    static std::atomic<uint32_t> trace_lines{0};
    // Entry 0 read from the pool itself (whatever sets it), each time it changes.
    {
      static float last0[3] = {0, 0, 0};
      static std::mutex m0;
      uint32_t pool0;
      {
        std::lock_guard lock(g_pools_mutex);
        pool0 = g_pools[0].base;
      }
      if (pool0 && Readable(base, pool0, 16)) {
        const float q[3] = {LoadF(base, pool0), LoadF(base, pool0 + 4), LoadF(base, pool0 + 8)};
        std::lock_guard lock(m0);
        if ((std::fabs(q[0] - last0[0]) > 0.001f || std::fabs(q[1] - last0[1]) > 0.001f ||
             std::fabs(q[2] - last0[2]) > 0.001f) &&
            q[0] > 88.0f && q[0] < 100.0f && q[1] > -6.0f && q[1] < 3.0f && q[2] > 12.0f && q[2] < 24.0f &&
            trace_lines.fetch_add(1, std::memory_order_relaxed) < 1500)
          REXLOG_WARN("[move_floor] trace: pool entry 0 ({:.4f} {:.4f} {:.4f})", q[0], q[1], q[2]);
        std::copy(q, q + 3, last0);
      }
    }
    if (pos[0] > 88.0f && pos[0] < 100.0f && pos[1] > -6.0f && pos[1] < 3.0f && pos[2] > 12.0f && pos[2] < 24.0f &&
        Readable(base, body + 8, 4)) {
      const uint32_t index = Load32(base, body + 8);
      if ((index == 0 || index == 2) && trace_lines.fetch_add(1, std::memory_order_relaxed) < 1500)
        REXLOG_WARN("[move_floor] trace: entry {} ({:.4f} {:.4f} {:.4f}); caller {:08X}", index, pos[0], pos[1], pos[2],
                    lr);
    }
    Movers(base);
  }
  if (!t_in_clip) {
    if (!g_have_pos.load(std::memory_order_relaxed))
      return;
    float d2 = 0;
    for (int i = 0; i < 3; ++i) {
      const float d = LoadF(base, p + 4 * i) - Bits(g_last_pos[i].load(std::memory_order_relaxed));
      d2 += d * d;
    }
    if (!(d2 <= 100.0f))
      return;
  }
  uint32_t since;
  Rate &rate = t_in_clip ? g_body_in : g_body_out;
  if (!rate.Take(since))
    return;
  REXLOG_WARN("[move_floor] body {:08X} pos ({:.3f} {:.3f} {:.3f}) {} {:08X}; caller {:08X}; {} sets since the last "
              "line of this kind",
              body, LoadF(base, p), LoadF(base, p + 4), LoadF(base, p + 8), t_in_clip ? "in clip" : "outside clips",
              t_clip, lr, since);
}

extern "C" REX_FUNC(sub_828A70C0) {
  if (!On()) {
    __imp__sub_828A70C0(ctx, base);
    return;
  }
  const uint32_t body = ctx.r3.u32, m = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  g_n_rot.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_828A70C0(ctx, base);
  uint32_t since;
  if (!m || !t_in_clip || !g_rot_in.Take(since))
    return;
  REXLOG_WARN("[move_floor] body {:08X} rotation rows ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} "
              "{:.3f}) in clip {:08X}; caller {:08X}; {} sets since the last line of this kind",
              body, LoadF(base, m), LoadF(base, m + 4), LoadF(base, m + 8), LoadF(base, m + 16), LoadF(base, m + 20),
              LoadF(base, m + 24), LoadF(base, m + 32), LoadF(base, m + 36), LoadF(base, m + 40), t_clip, lr, since);
}

// tw2de (2026-11-02): the dock's collision boxes end up on or off the wrong way round at 30 fps (pose entry +102
// bit 0x1000: entry 278 off and 488 / 489 on where the boat can be boarded the other way round). Every change of a
// body at the dock (or of those three entries) is logged with the guest callers, so that the order of the game's
// on / off calls can be compared between a 30 fps and a 20 fps run.
namespace {
std::string GuestCallers(const uint8_t *base, uint32_t lr, uint32_t sp) {
  std::string s = fmt::format("LR {:08X}, stack", lr);
  for (int k = 0; k < 8 && sp; ++k) {
    if (!Readable(base, sp, 4))
      break;
    const uint32_t next = Load32(base, sp);
    if (next <= sp || next - sp > 0x100000 || !Readable(base, next - 8, 4))
      break;
    s += fmt::format(" {:08X}", Load32(base, next - 8));
    sp = next;
  }
  return s;
}
std::atomic<uint32_t> g_enable_lines{0};
}  // namespace

extern "C" REX_FUNC(sub_828A7200) {
  if (!On()) {
    __imp__sub_828A7200(ctx, base);
    return;
  }
  const uint32_t body = ctx.r3.u32, on = ctx.r4.u32 & 0xFF, lr = static_cast<uint32_t>(ctx.lr), sp = ctx.r1.u32;
  uint32_t entry = 0xFFFFFFFFu, e = 0;
  bool before = false;
  if (body && Readable(base, body + 8, 8)) {
    const uint32_t holder = Load32(base, body + 0xC);
    entry = Load32(base, body + 8);
    if (holder && entry < 0x4000 && Readable(base, holder, 4)) {
      e = Load32(base, holder) + entry * 0x70;
      if (!Readable(base, e, 0x70))
        e = 0;
    }
  }
  if (e)
    before = (Load32(base, e + 100) & 0x1000) != 0;
  __imp__sub_828A7200(ctx, base);
  if (!e)
    return;
  const float x = LoadF(base, e), y = LoadF(base, e + 4), z = LoadF(base, e + 8);
  if (!AtDock(x, y, z) && entry != 278 && entry != 488 && entry != 489)
    return;
  const bool after = (Load32(base, e + 100) & 0x1000) != 0;
  if (after == before)
    return;  // tw2de (2026-11-02): only changes (the same state set every frame filled the log)
  if (g_enable_lines.fetch_add(1, std::memory_order_relaxed) >= 4000)
    return;
  REXLOG_WARN("[move_floor] collision {}: body {:08X} entry {} at ({:.3f} {:.3f} {:.3f}) was {} now {}; {}",
              on ? "ON" : "OFF", body, entry, x, y, z, before ? "on" : "off", after ? "on" : "off",
              GuestCallers(base, lr, sp));
}

// tw2de (2026-11-02, diag9): why Lightning's move is undone at the boat's edge. Her controller (r3) moves her in
// 0.02 steps (sub_82404AA0, recursively); each step first asks the physics for her contacts (sub_828A80B8) into two
// lists (controller +0x44 and +0xA0: a chunk of 64 records of 48 bytes from +0x20, the count at chunk +8), then
// resolves them (sub_824033A8). Then sub_82403C98(controller, position) sets her final position, and when blocked
// that is the frame's start again. With tw2de_diag_move_floor, in x 88..100, y -6..3, z 12..24 (3000 lines max):
//   [move_floor] contacts: controller, position, both counts and the first four records of each (12 words)
//   [move_floor] final: controller, the position asked, flags +0x40, the position set, caller
REX_EXTERN(__imp__sub_824033A8);
REX_EXTERN(__imp__sub_82403C98);

namespace {
std::atomic<uint32_t> g_diag9_lines{0};

bool ControllerPos(const uint8_t *base, uint32_t ctrl, float pos[3]) {
  if (!Readable(base, ctrl, 0xA4))
    return false;
  const uint32_t body = Load32(base, ctrl + 0x2c);
  if (!body || !Readable(base, body + 8, 8))
    return false;
  const uint32_t index = Load32(base, body + 8), holder = Load32(base, body + 0xC);
  if (!holder || index > 0x4000 || !Readable(base, holder, 4))
    return false;
  const uint32_t pool = Load32(base, holder);
  if (!pool || !Readable(base, pool + index * 0x70, 16))
    return false;
  for (int i = 0; i < 3; ++i)
    pos[i] = LoadF(base, pool + index * 0x70 + 4 * i);
  return true;
}

bool InDock(const float p[3]) {
  return p[0] > 88.0f && p[0] < 100.0f && p[1] > -6.0f && p[1] < 3.0f && p[2] > 12.0f && p[2] < 24.0f;
}

std::string Records(const uint8_t *base, uint32_t head) {
  if (!head || !Readable(base, head, 12))
    return "none";
  const uint32_t count = Load32(base, head + 8);
  std::string s = fmt::format("{}", count);
  for (uint32_t i = 0; i < count && i < 4; ++i) {
    const uint32_t rec = head + i * 48 + 0x20;
    if (!Readable(base, rec, 48))
      break;
    s += fmt::format(" [{}:", i);
    for (int w = 0; w < 12; ++w) {
      const uint32_t v = Load32(base, rec + 4 * w);
      float f;
      std::memcpy(&f, &v, 4);
      if (std::fabs(f) > 1e-6f && std::fabs(f) < 1e5f)
        s += fmt::format(" {:.4f}", f);
      else
        s += fmt::format(" {:08X}", v);
    }
    s += "]";
  }
  return s;
}
}  // namespace

extern "C" REX_FUNC(sub_824033A8) {
  if (On()) {
    const uint32_t ctrl = ctx.r3.u32;
    float p[3];
    if (ControllerPos(base, ctrl, p) && InDock(p) &&
        g_diag9_lines.fetch_add(1, std::memory_order_relaxed) < 3000)
      REXLOG_WARN("[move_floor] contacts: controller {:08X} at ({:.4f} {:.4f} {:.4f}) r5 {}; +44: {}; +A0: {}", ctrl,
                  p[0], p[1], p[2], ctx.r5.u32, Records(base, Load32(base, ctrl + 0x44)),
                  Records(base, Load32(base, ctrl + 0xA0)));
  }
  __imp__sub_824033A8(ctx, base);
}

extern "C" REX_FUNC(sub_82403C98) {
  if (!On()) {
    __imp__sub_82403C98(ctx, base);
    return;
  }
  const uint32_t ctrl = ctx.r3.u32, a = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  float asked[3] = {0, 0, 0};
  if (a && Readable(base, a, 12))
    for (int i = 0; i < 3; ++i)
      asked[i] = LoadF(base, a + 4 * i);
  const uint32_t flags = Readable(base, ctrl, 0x44) ? Load32(base, ctrl + 0x40) : 0;
  float before[3] = {0, 0, 0};
  const bool had = ControllerPos(base, ctrl, before);
  __imp__sub_82403C98(ctx, base);
  float after[3] = {0, 0, 0};
  if (!ControllerPos(base, ctrl, after) || !(InDock(after) || (had && InDock(before))))
    return;
  if (g_diag9_lines.fetch_add(1, std::memory_order_relaxed) < 3000)
    REXLOG_WARN("[move_floor] final: controller {:08X} asked ({:.4f} {:.4f} {:.4f}) flags {:08X} was ({:.4f} {:.4f} "
                "{:.4f}) set ({:.4f} {:.4f} {:.4f}); caller {:08X}",
                ctrl, asked[0], asked[1], asked[2], flags, before[0], before[1], before[2], after[0], after[1],
                after[2], lr);
}

namespace {
thread_local bool t_cast_trace = false;
thread_local int t_cast_depth = 0;
thread_local uint32_t t_cast_lines = 0;
}  // namespace

// tw2de (2026-11-02, diag10): the ground check. sub_82403C98's sub_824028B8 casts down for the floor under the
// controller (sub_828A2258(out, world, ray: start +0 / direction +16, filter*, f1 length, ..., r10 self)); with no
// floor (hit record +52 = 0) it sets the controller's flag 0x80 and her position goes back to the last one with a
// floor (sub_82403C98 at 0x824046C8). Lightning is reverted that way at the boat's edge on the Switch. With
// tw2de_diag_move_floor, each such cast whose start is at the dock (x 88..100, y -6..3, z 12..24):
//   [move_floor] ray: caller, start, direction, length, filter, self -> hit object, +32, +48, point, normal
REX_EXTERN(__imp__sub_828A2258);

extern "C" REX_FUNC(sub_828A2258) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (!On() || lr < 0x824028B8 || lr > 0x824031AC) {
    __imp__sub_828A2258(ctx, base);
    return;
  }
  const uint32_t ray = ctx.r5.u32, filter = ctx.r6.u32, self = ctx.r10.u32;
  const float length = static_cast<float>(ctx.f1.f64);
  float s[8] = {};
  const bool ok = ray && Readable(base, ray, 32);
  if (ok)
    for (int i = 0; i < 8; ++i)
      s[i] = LoadF(base, ray + 4 * i);
  const uint32_t filt = filter && Readable(base, filter, 4) ? Load32(base, filter) : 0;
  // diag12: the first 8 floor casts around the hole (x 93.75..94.0, z 17.4..18.1) are traced call by call.
  static std::atomic<uint32_t> traced{0};
  const bool trace = ok && lr == 0x82402AF4 && s[0] > 93.75f && s[0] < 94.0f && s[2] > 17.4f && s[2] < 18.1f &&
                     traced.fetch_add(1, std::memory_order_relaxed) < 8;
  if (trace) {
    REXLOG_WARN("[move_floor] cast trace begin: start ({:.4f} {:.4f} {:.4f})", s[0], s[1], s[2]);
    t_cast_trace = true;
    t_cast_depth = 0;
    t_cast_lines = 0;
  }
  __imp__sub_828A2258(ctx, base);
  if (trace) {
    t_cast_trace = false;
    REXLOG_WARN("[move_floor] cast trace end: object {:08X} distance {:.4f} ({} calls)", Load32(base, ctx.r3.u32 + 52),
                LoadF(base, ctx.r3.u32 + 32), t_cast_lines);
  }
  if (!ok || !InDock(s))
    return;
  const uint32_t out = ctx.r3.u32;
  if (!out || !Readable(base, out, 56))
    return;
  float h[8];
  for (int i = 0; i < 8; ++i)
    h[i] = LoadF(base, out + 4 * i);
  if (g_diag9_lines.fetch_add(1, std::memory_order_relaxed) < 3000)
    REXLOG_WARN("[move_floor] ray: caller {:08X} start ({:.4f} {:.4f} {:.4f}) dir ({:.4f} {:.4f} {:.4f}) length {:.4f} "
                "filter {:08X} self {:08X} -> object {:08X} +32 {:.4f} +48 {:.4f} +36 {:08X} +40 {:08X} point ({:.4f} "
                "{:.4f} {:.4f}) normal ({:.4f} {:.4f} {:.4f})",
                lr, s[0], s[1], s[2], s[4], s[5], s[6], length, filt, self, Load32(base, out + 52),
                LoadF(base, out + 32), LoadF(base, out + 48), Load32(base, out + 36), Load32(base, out + 40), h[0],
                h[1], h[2], h[4], h[5], h[6]);
}

// tw2de (2026-11-02, diag12): the functions a floor cast runs (its static call tree, sub_828A2258 down; the
// two native vmath ones excepted), each traced while a traced cast runs on this thread: depth, name, caller,
// r3..r7, f1..f3 in, r3 and f1 out (at most 1500 calls a cast).
namespace {
void CastTraceLine(const char *name, int depth, uint32_t lr, const uint32_t (&in)[5], const double (&fin)[3], uint32_t r3,
                   double f1) {
  if (++t_cast_lines > 1500)
    return;
  REXLOG_WARN("[move_floor] cast {:>{}}{} lr {:08X} r3..r7 {:08X} {:08X} {:08X} {:08X} {:08X} f1..f3 {:.5f} {:.5f} {:.5f} -> "
              "r3 {:08X} f1 {:.5f}",
              "", depth * 2, name, lr, in[0], in[1], in[2], in[3], in[4], fin[0], fin[1], fin[2], r3, f1);
}
}  // namespace

#define FNXF_CAST_TRACE(addr)                                                                     \
  REX_EXTERN(__imp__sub_##addr);                                                                  \
  extern "C" REX_FUNC(sub_##addr) {                                                               \
    if (!t_cast_trace) {                                                                          \
      __imp__sub_##addr(ctx, base);                                                               \
      return;                                                                                     \
    }                                                                                             \
    const uint32_t in[5] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};           \
    const double fin[3] = {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64};                                   \
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);                                            \
    const int depth = t_cast_depth++;                                                             \
    __imp__sub_##addr(ctx, base);                                                                 \
    t_cast_depth = depth;                                                                         \
    CastTraceLine("sub_" #addr, depth, lr, in, fin, ctx.r3.u32, ctx.f1.f64);                      \
  }

FNXF_CAST_TRACE(828AF390)
FNXF_CAST_TRACE(828AE8C0)
FNXF_CAST_TRACE(828AE1A8)
FNXF_CAST_TRACE(828AB438)
FNXF_CAST_TRACE(8222C7B8)
FNXF_CAST_TRACE(828A9458)
FNXF_CAST_TRACE(828A93C8)
FNXF_CAST_TRACE(828AAF28)
FNXF_CAST_TRACE(828ADEF8)
FNXF_CAST_TRACE(828AD698)
FNXF_CAST_TRACE(828ABAE8)
FNXF_CAST_TRACE(828AC600)
FNXF_CAST_TRACE(828AB988)
FNXF_CAST_TRACE(828A9340)
FNXF_CAST_TRACE(828AD728)
FNXF_CAST_TRACE(828AC9B8)
FNXF_CAST_TRACE(828A94C8)
FNXF_CAST_TRACE(828A96B0)
FNXF_CAST_TRACE(828B7090)
FNXF_CAST_TRACE(828B4ED0)
FNXF_CAST_TRACE(828B42A0)
FNXF_CAST_TRACE(828B3F90)
FNXF_CAST_TRACE(828B6FE8)
FNXF_CAST_TRACE(828B68C0)
FNXF_CAST_TRACE(828FC2A0)
FNXF_CAST_TRACE(828B2BC0)
FNXF_CAST_TRACE(828B2998)
FNXF_CAST_TRACE(828B3518)
FNXF_CAST_TRACE(828B3388)
FNXF_CAST_TRACE(82227AB0)
FNXF_CAST_TRACE(828B4790)
FNXF_CAST_TRACE(828B54E0)
FNXF_CAST_TRACE(828B0450)
FNXF_CAST_TRACE(828B3840)
FNXF_CAST_TRACE(828B2B60)
FNXF_CAST_TRACE(828B3468)
FNXF_CAST_TRACE(828B31B0)
FNXF_CAST_TRACE(828B3EF8)
FNXF_CAST_TRACE(828B2B00)
FNXF_CAST_TRACE(828B3690)
FNXF_CAST_TRACE(828B35D8)
FNXF_CAST_TRACE(828ED658)
FNXF_CAST_TRACE(828B4190)
FNXF_CAST_TRACE(822310E8)
FNXF_CAST_TRACE(828B4990)
FNXF_CAST_TRACE(828EABB0)
FNXF_CAST_TRACE(828E9B78)
FNXF_CAST_TRACE(828EDD60)
FNXF_CAST_TRACE(828EDBC8)
FNXF_CAST_TRACE(828ED9E8)
FNXF_CAST_TRACE(828EDFE0)
FNXF_CAST_TRACE(828ED8F0)
FNXF_CAST_TRACE(828ED848)
FNXF_CAST_TRACE(828ED498)
FNXF_CAST_TRACE(828B4228)
FNXF_CAST_TRACE(828FC7F0)

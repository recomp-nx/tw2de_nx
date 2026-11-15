// tw2de (2026-11-01, tw2de_diag_mesh_proxy): the physics' mesh proxies, for diagnostics.
//
// The CDev framework's PhysicsController::MeshProxy reads a model's vertices out of its vertex buffers for the physics
// (GetVertices, sub_82739C28, slot 21 of its vtable at 0x82108DA0: it locks each vertex buffer for reading, logs
// "PhysicsController:Vertex buffer lock(read) error." when a lock fails, decodes the vertices asked for into r5's
// three arrays, r4, r5 and r6, 16 bytes a vertex each: position, normal and another of its streams) and writes vertices back (sub_827391E8, slot 22, "lock(write) error."). If a moving floor's
// collision is made from vertices the GPU writes, what the physics reads depends on the GPU path, which on the Switch
// is Renut's, not Xenia's. With the cvar: for each of the two, at most two lines a second, the call's arguments, the
// first three vertices it returned (GetVertices) and its caller, and how many calls there were since the last line.
// Off (the default): both run as before, a cvar read more.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(tw2de_diag_mesh_proxy, false, "Game/Diagnostics",
                    "Diagnostic: log the physics reading and writing model vertices (PhysicsController::MeshProxy)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82739C28);  // MeshProxy::GetVertices
REX_EXTERN(__imp__sub_827391E8);  // the vertex write

namespace {
float LoadF(const uint8_t *base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, base + ea, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// At most two lines a second per hook; the calls in between counted.
struct Rate {
  std::atomic<int64_t> next{0};
  std::atomic<uint32_t> calls{0};
  // True when this call may log; `since` the calls since the last line, this one included.
  bool Take(uint32_t &since) {
    since = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const int64_t now = NowMs();
    int64_t n = next.load(std::memory_order_relaxed);
    if (now < n || !next.compare_exchange_strong(n, now + 500, std::memory_order_relaxed))
      return false;
    calls.fetch_sub(since, std::memory_order_relaxed);
    return true;
  }
};
Rate g_get, g_set;

bool On() {
  static const bool on = REXCVAR_GET(tw2de_diag_mesh_proxy);
  return on;
}
}  // namespace

extern "C" REX_FUNC(sub_82739C28) {
  if (!On()) {
    __imp__sub_82739C28(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32, r4 = ctx.r4.u32, out = ctx.r5.u32, r6 = ctx.r6.u32, mesh = ctx.r7.u32,
                 r8 = ctx.r8.u32, idx = ctx.r9.u32, count = ctx.r10.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82739C28(ctx, base);
  const uint32_t ok = ctx.r3.u32;
  uint32_t since;
  if (!g_get.Take(since))
    return;
  // The first vertex of each output array, and the second of r4's.
  float v[4][3] = {};
  const uint32_t at[4] = {r4, out, r6, r4 + 16};
  if (ok && count)
    for (int k = 0; k < 4; ++k)
      if (at[k] && (k < 3 || count > 1))
        for (int i = 0; i < 3; ++i)
          v[k][i] = LoadF(base, at[k] + 4 * i);
  REXLOG_WARN("[mesh_proxy] get: proxy {:08X} mesh {} count {} -> {} (r4 {:08X}, r5 {:08X}, r6 {:08X}, r8 {:08X}, "
              "indices {:08X}); vertex 0: r4 ({:.3f} {:.3f} {:.3f}) r5 ({:.3f} {:.3f} {:.3f}) r6 ({:.3f} {:.3f} "
              "{:.3f}); vertex 1: r4 ({:.3f} {:.3f} {:.3f}); caller {:08X}; {} calls since the last line",
              self, int32_t(mesh), count, ok, r4, out, r6, r8, idx, v[0][0], v[0][1], v[0][2], v[1][0], v[1][1],
              v[1][2], v[2][0], v[2][1], v[2][2], v[3][0], v[3][1], v[3][2], lr, since);
}

extern "C" REX_FUNC(sub_827391E8) {
  if (!On()) {
    __imp__sub_827391E8(ctx, base);
    return;
  }
  const uint32_t a[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                         ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_827391E8(ctx, base);
  uint32_t since;
  if (!g_set.Take(since))
    return;
  REXLOG_WARN("[mesh_proxy] set: r3..r10 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> {}; caller {:08X}; "
              "{} calls since the last line",
              a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], ctx.r3.u32, lr, since);
}

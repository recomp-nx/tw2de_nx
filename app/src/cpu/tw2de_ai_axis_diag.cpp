// tw2de (2026-11-01, tw2de_diag_ai_axis): the field AI's angle function, for diagnostics.
//
// sub_82308950(ai, axis, other, out1, out2) (called from sub_823093A8, FieldAiMain.cpp's code) takes a row of a
// matrix (axis: sub_82307CA8 picks the row at +0, +16 or +32), normalizes it with one lane replaced, and gives acos of
// its dot product with the row as *out1 / *out2. A row longer than 1 makes that acos's argument above 1 and the angle
// NaN: in the boat scene (z008) fp_origin_log saw the game's asin/acos (sub_82972928) take the square root of -0.0016,
// an argument of 1.0032, called from here. With the cvar, a line (at most two a second) for each call whose row is
// not of length 1 within 0.001 or whose angles come out NaN: the row, its length, the other vector, the angles, the
// caller. Off (the default): sub_82308950 runs as before, a cvar read more.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(tw2de_diag_ai_axis, false, "Game/Diagnostics",
                    "Diagnostic: log the field AI's angle function (sub_82308950) when its matrix row is not of length "
                    "1 or its angles are NaN")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82308950);

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
}  // namespace

extern "C" REX_FUNC(sub_82308950) {
  static const bool on = REXCVAR_GET(tw2de_diag_ai_axis);
  if (!on) {
    __imp__sub_82308950(ctx, base);
    return;
  }
  const uint32_t ai = ctx.r3.u32, axis = ctx.r4.u32, other = ctx.r5.u32, out1 = ctx.r6.u32, out2 = ctx.r7.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  float a[4] = {}, b[4] = {};
  if (axis)
    for (int i = 0; i < 4; ++i)
      a[i] = LoadF(base, axis + 4 * i);
  if (other)
    for (int i = 0; i < 4; ++i)
      b[i] = LoadF(base, other + 4 * i);
  __imp__sub_82308950(ctx, base);
  const float o1 = out1 ? LoadF(base, out1) : 0.0f, o2 = out2 ? LoadF(base, out2) : 0.0f;
  const double len = std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]);
  static uint32_t calls = 0, bad = 0;
  ++calls;
  if (std::fabs(len - 1.0) <= 0.001 && !std::isnan(o1) && !std::isnan(o2))
    return;
  ++bad;
  static int64_t next = 0;
  const int64_t now = NowMs();
  if (now < next)
    return;
  next = now + 500;
  REXLOG_WARN("[ai_axis] ai {:08X} row {:08X} ({:.6f} {:.6f} {:.6f} {:.6f}) length {:.6f}, other {:08X} ({:.6f} {:.6f} "
              "{:.6f} {:.6f}), angles {:.6f} {:.6f}, caller {:08X}; {} of {} calls so far",
              ai, axis, a[0], a[1], a[2], a[3], len, other, b[0], b[1], b[2], b[3], o1, o2, lr, bad, calls);
}

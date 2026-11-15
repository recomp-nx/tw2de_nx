// TW2 (2026-11-14): FMOD Ex's reverb (sub_82968720, the SFX reverb's DSP read: 940 PowerPC instructions, about 950
// executed a sample) as native code. It was the largest single function of the game's audio: 47.6% of the FMOD mixer
// thread's samples in the Tutorial scene's profile (11.5% of a core at the stock clocks), 10-18% of a core in the
// prologue's.
//
// What it computes, per sample of a block (the same operations, in the same order, on locals instead of the context):
//   - the block's input, mixed down to one channel by sub_829672A0 (still the game's code, called from here), through
//     two one-pole low-passes and an optional biquad, written into the early-reflections delay line;
//   - tap 0 of that line through two all-passes into an eight-tap delay line: the eight "diffused" values;
//   - eight late lines: each takes one more early tap, its own delayed sample times its gain, a low-pass, and all
//     eight states go through a rotation matrix (VMX there, NEON here) back into the lines;
//   - every output channel in the speaker mask: the sum of its lines' diffused values and output taps, plus the dry
//     input times the dry gain.
// Delays, gains and coefficients that changed since the last block are cross-faded over the block (old * (n - i) +
// new * i) / n, as there.
//
// The arithmetic is the generated code's: fmuls / fadds / fsubs are single-precision operations (the generated code
// computes them in double and rounds to single, which gives the same value for single operands: the product is exact
// in double, and for a sum the two roundings are innocuous at 53 >= 2 * 24 + 2 bits); fmadds / fnmsubs are the fused
// double result rounded to single, computed that way here too; vmaddfp is the fused single NEON form the SDK uses.
// Every stored value and every store card is the same for finite inputs (tools/native_reverb_test/run.sh: the
// original and this on the same random reverb objects under qemu-aarch64, the memory window and its cards compared;
// 120,000 objects, 2.77 million samples, 1 to 8 channels, 0 to 8 lines, with and without cross-fades, with denormals,
// the output over the input). NaNs are not promised to come out with the same payload.
//
// Left to the original (Reverb returns false, nothing touched): more than 8 late lines, fewer than 1 or more than 8
// channels, the state or work vectors not on 16 bytes or over the object's own fields; the caller also leaves it to
// the original on a host that switches the flush mode (switch_flush_mode_fixed off): this code never writes FPCR.
#pragma once

#include <arm_neon.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/ppc.h>

extern "C" void __imp__sub_829672A0(PPCContext &__restrict ctx, uint8_t *base);  // the mix-down to one channel

namespace tw2de::native_reverb {

inline uint32_t LU(const uint8_t *base, uint32_t ea) {
  uint32_t u;
  std::memcpy(&u, base + ea, 4);
  return __builtin_bswap32(u);
}
inline float LF(const uint8_t *base, uint32_t ea) {
  const uint32_t u = LU(base, ea);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline void Card(uint8_t *base, uint32_t ea) { base[ea >> 12] = 0; }  // the store card (REX_CARD)
inline void SU(uint8_t *base, uint32_t ea, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + ea, &v, 4);
  Card(base, ea);
}
inline void SF(uint8_t *base, uint32_t ea, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  SU(base, ea, u);
}
inline void SB(uint8_t *base, uint32_t ea, uint8_t v) {
  base[ea] = v;
  Card(base, ea);
}
// lvx128 / stvx128: the address rounded down to 16, lane i = the guest's word i.
inline float32x4_t LV(const uint8_t *base, uint32_t ea) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(base + (ea & ~0xFu))));
}
inline void SV(uint8_t *base, uint32_t ea, float32x4_t v) {
  ea &= ~0xFu;
  vst1q_u8(base + ea, vrev32q_u8(vreinterpretq_u8_f32(v)));
  Card(base, ea);
}
// fmadds: a * b + c, fused in double, rounded to single (the generated code's double(float(std::fma(...)))).
inline float Fma(float a, float b, float c) { return float(std::fma(double(a), double(b), double(c))); }
// fnmsubs: -(a * b - c), fused in double, rounded to single (rex::ppc::fnmsub: a zero result has this form's sign).
inline float Fnms(float a, float b, float c) {
  const double t = std::fma(double(a), double(b), -double(c));
  return float(t != t ? t : -t);  // a NaN keeps its sign there
}

// One parameter's cross-fade over the block: (old * (n - i) + i * new) / n, in the original's operations.
struct Fade {
  float fi, fr, inv;
  float operator()(float now, float old) const { return Fma(old, fr, fi * now) * inv; }
};

inline bool Reverb(PPCContext &ctx, uint8_t *base) {
  const uint32_t self = ctx.r3.u32, in = ctx.r4.u32, out = ctx.r5.u32, len = ctx.r7.u32;
  const int32_t channels = ctx.r6.s32;
  const uint32_t speakers = ctx.r10.u16;
  const double dry = ctx.f2.f64;
  const int32_t lines = int32_t(LU(base, self + 720));
  if (channels < 1 || channels > 8 || lines > 8)
    return false;
  // The state (8 floats) and work (6 vectors) areas: wherever they are, not over the fields kept in locals here.
  const uint32_t state = LU(base, self + 500), work = LU(base, self + 504);
  const auto apart = [](uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a + an <= b || b + bn <= a; };
  if ((state & 0xF) != 0 || (work & 0xF) != 0 || !apart(state, 32, work, 96) || !apart(state, 32, self, 244) ||
      !apart(state, 32, self + 500, 952) || !apart(work, 96, self, 244) || !apart(work, 96, self + 500, 952))
    return false;

  // 1. The input's alternating offset (+1e-15 on even frames, -1e-15 on odd ones: no denormals in the lines). The 6
  //    and 8 channel forms work on whole vectors, four frames a turn, and leave the frames past a multiple of four.
  if (channels == 6) {
    const float32x4_t plus = LV(base, 0x820F7970), both = LV(base, 0x820F7980), minus = LV(base, 0x820F7960);
    uint32_t p = in;
    for (uint32_t n = len >> 2; n; --n, p += 96) {
      SV(base, p, vaddq_f32(LV(base, p), plus));
      SV(base, p + 16, vaddq_f32(LV(base, p + 16), both));
      SV(base, p + 32, vaddq_f32(LV(base, p + 32), minus));
      SV(base, p + 48, vaddq_f32(LV(base, p + 48), plus));
      SV(base, p + 64, vaddq_f32(LV(base, p + 64), both));
      SV(base, p + 80, vaddq_f32(LV(base, p + 80), minus));
    }
  } else if (channels == 8) {
    const float32x4_t plus = LV(base, 0x820F7970), minus = LV(base, 0x820F7960);
    uint32_t p = in;
    for (uint32_t n = len >> 2; n; --n, p += 128) {
      SV(base, p, vaddq_f32(LV(base, p), plus));
      SV(base, p + 16, vaddq_f32(LV(base, p + 16), plus));
      SV(base, p + 32, vaddq_f32(LV(base, p + 32), minus));
      SV(base, p + 48, vaddq_f32(LV(base, p + 48), minus));
      SV(base, p + 64, vaddq_f32(LV(base, p + 64), plus));
      SV(base, p + 80, vaddq_f32(LV(base, p + 80), plus));
      SV(base, p + 96, vaddq_f32(LV(base, p + 96), minus));
      SV(base, p + 112, vaddq_f32(LV(base, p + 112), minus));
    }
  } else {
    const float offset = LF(base, 0x820F7958);
    const uint32_t stride = uint32_t(channels) * 8;
    for (int32_t c = 0; c < channels; ++c) {
      if (len != 0) {
        uint32_t p = in + uint32_t(c) * 4;
        for (uint32_t n = ((len - 1) >> 1) + 1; n; --n, p += stride)
          SF(base, p, LF(base, p) + offset);
      }
      if (len > 1) {
        uint32_t p = in + uint32_t(channels) * 4 + uint32_t(c) * 4;
        for (uint32_t n = ((len - 2) >> 1) + 1; n; --n, p += stride)
          SF(base, p, LF(base, p) - offset);
      }
    }
  }

  // 2. The block mixed down to one channel (the object's buffer at +4): the game's function, with the arguments the
  //    original passes. Its frame goes below this caller's stack pointer, where the original's own frame would be.
  {
    const uint64_t a_in = ctx.r4.u64, a_channels = ctx.r6.u64, a_len = ctx.r7.u64;
    ctx.r6.u64 = a_in;
    ctx.r5.u64 = a_channels;
    ctx.r4.u64 = a_len;
    ctx.lr = 0x82968990;
  }
  __imp__sub_829672A0(ctx, base);

  // 3. What changed since the last block: the read positions of every tap from the write positions, and for each
  //    changed delay, gain or coefficient the old value beside the new one and its flag.
  const uint32_t er_mask = LU(base, self + 1216);
  uint32_t er_write = LU(base, self + 1220);
  uint32_t er_now[9], er_old[9];
  bool er_fade[9];
  for (uint32_t i = 0; i < 9; ++i) {
    const uint32_t target = LU(base, self + 1128 + 4 * i), current = LU(base, self + 1164 + 4 * i);
    er_now[i] = (target + er_write - 1) & er_mask;
    SU(base, self + 1224 + 4 * i, er_now[i]);
    er_old[i] = 0;
    if (target == current) {
      er_fade[i] = false;
      SB(base, self + 1200 + i, 0);
    } else {
      er_fade[i] = true;
      SB(base, self + 1200 + i, 1);
      SU(base, self + 1164 + 4 * i, target);
      er_old[i] = (current + er_write - 1) & er_mask;
      SU(base, self + 1260 + 4 * i, er_old[i]);
    }
  }
  uint32_t l_buf[8], l_mask[8], l_write[8], l_now[8], l_old[8], l_out[8];
  bool l_fade[8], c_fade[8];
  float g_now[8], g_old[8], c_now[8], c_rest[8], c_old[8];
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t f = self + 788 + 4 * i;
    l_buf[i] = LU(base, self + 724 + 4 * i);
    l_write[i] = LU(base, f + 168);
    l_mask[i] = LU(base, f + 136);
    const uint32_t target = LU(base, f), current = LU(base, f + 32);
    l_now[i] = (l_write[i] + target - 1) & l_mask[i];
    l_out[i] = (l_write[i] + LU(base, f + 64) - 1) & l_mask[i];
    SU(base, f + 200, l_now[i]);
    SU(base, f + 264, l_out[i]);
    l_old[i] = 0;
    if (target == current) {
      l_fade[i] = false;
      SB(base, self + 916 + i, 0);
    } else {
      l_fade[i] = true;
      SB(base, self + 916 + i, 1);
      SU(base, f + 32, target);
      l_old[i] = (l_write[i] + current - 1) & l_mask[i];
      SU(base, f + 232, l_old[i]);
    }
    // the line's gain
    float g = LF(base, self + 508 + 4 * i);
    const float g_current = LF(base, self + 572 + 4 * i);
    SF(base, self + 540 + 4 * i, g);
    g_now[i] = g;
    if (!(g == g_current)) {
      if (!l_fade[i]) {
        l_fade[i] = true;
        SB(base, self + 916 + i, 1);
        l_old[i] = (l_write[i] + LU(base, f + 32) - 1) & l_mask[i];
        SU(base, f + 232, l_old[i]);
      }
      SF(base, self + 572 + 4 * i, g);
      g = g_current;
    }
    SF(base, self + 604 + 4 * i, g);
    g_old[i] = g;
    // the line's low-pass coefficient
    const float c = LF(base, self + 76 + 4 * i), c_current = LF(base, self + 212 + 4 * i);
    c_now[i] = c;
    c_rest[i] = 1.0f - c;
    c_old[i] = 0;
    SF(base, self + 116 + 4 * i, c);
    SF(base, self + 148 + 4 * i, c_rest[i]);
    if (c == c_current) {
      c_fade[i] = false;
      SB(base, self + 108 + i, 0);
    } else {
      c_fade[i] = true;
      SB(base, self + 108 + i, 1);
      c_old[i] = c_current;
      SF(base, self + 180 + 4 * i, c_current);
      SF(base, self + 212 + 4 * i, c);
    }
  }
  // the five single parameters: target, the value in use, the old one, the one kept for the next block, the flag
  struct One {
    float now, old;
    bool fade;
  };
  const auto one = [base, self](uint32_t target, uint32_t now, uint32_t old, uint32_t kept, uint32_t flag) {
    One p;
    p.now = LF(base, self + target);
    p.old = LF(base, self + kept);
    SF(base, self + now, p.now);
    if (p.now == p.old) {
      p.fade = false;
      SB(base, self + flag, 0);
    } else {
      p.fade = true;
      SF(base, self + old, p.old);
      SB(base, self + flag, 1);
      SF(base, self + kept, p.now);
    }
    return p;
  };
  const One early = one(660, 668, 672, 676, 664);    // the early taps into the late lines
  const One first = one(656, 684, 688, 692, 680);    // tap 0 into the all-passes
  const One input = one(16, 24, 28, 32, 20);         // the input low-passes' coefficient
  const One spread = one(636, 644, 648, 652, 640);   // the rotation of the lines' states
  const One late = one(696, 704, 708, 712, 700);     // the late lines' input gain
  if (len == 0)
    return true;

  // 4. The block.
  const uint32_t mono = LU(base, self + 4);
  float lp1 = LF(base, self + 36), lp2 = LF(base, self + 40);
  const bool shelf = !(LF(base, self + 64) == 0.0f);
  const float b0 = LF(base, self + 44), b1 = LF(base, self + 48), b2 = LF(base, self + 52), a1 = LF(base, self + 56),
              a2 = LF(base, self + 60);
  float z1 = LF(base, self + 68), z2 = LF(base, self + 72);
  const uint32_t er_buf = LU(base, self + 1084);
  const float ap_gain = LF(base, self + 716);
  const uint32_t ap1_buf = LU(base, self + 1396), ap2_buf = LU(base, self + 1400);
  const uint32_t ap1_mask = LU(base, self + 1428), ap2_mask = LU(base, self + 1432);
  uint32_t ap1_write = LU(base, self + 1436), ap2_write = LU(base, self + 1440);
  uint32_t ap1_read = LU(base, self + 1444), ap2_read = LU(base, self + 1448);
  const uint32_t taps_buf = LU(base, self + 1296), taps_mask = LU(base, self + 1360);
  uint32_t taps_pos[8];  // [0] the write position, [1..7] the seven read positions
  for (uint32_t k = 0; k < 8; ++k)
    taps_pos[k] = LU(base, self + 1364 + 4 * k);
  float st[8];
  for (uint32_t j = 0; j < 8; ++j)
    st[j] = LF(base, state + 4 * j);
  const bool any_fade = [&] {
    bool any = early.fade || first.fade || input.fade || spread.fade || late.fade;
    for (uint32_t i = 0; i < 9; ++i)
      any = any || er_fade[i];
    for (uint32_t i = 0; i < 8; ++i)
      any = any || l_fade[i] || c_fade[i];
    return any;
  }();
  const float inv = 1.0f / float(double(int64_t(len)));
  const uint32_t per_channel = uint32_t(8 / channels);  // the lines summed into one output channel
  bool out_used[8] = {};
  float32x4_t w0{}, w1{}, w2{}, w3{}, m_lo{}, m_hi{};
  float diffused[8];
  uint32_t frame_out = out;

  for (uint32_t i = 0; i < len; ++i, frame_out += uint32_t(channels) * 4) {
    Fade fade{0, 0, inv};
    if (any_fade) {
      fade.fi = float(double(int64_t(i)));
      fade.fr = float(double(int64_t(len - i)));
    }
    // the input's two low-passes and the biquad, into the early line
    float x = LF(base, mono + 4 * i);
    const float c = input.fade ? fade(input.now, input.old) : input.now;
    const float d = spread.fade ? fade(spread.now, spread.old) : spread.now;
    const float e = late.fade ? fade(late.now, late.old) : late.now;
    const float rest = 1.0f - c;
    lp1 = Fma(lp1, rest, c * x);
    lp2 = Fma(lp2, rest, lp1 * c);
    if (shelf) {
      const float t = Fma(b0, lp2, z1);
      const float n1 = Fma(b1, lp2, a1 * t);
      const float n2 = Fma(b2, lp2, a2 * t);
      z1 = n1 + z2;
      z2 = n2;
      x = t;
    } else {
      x = lp2;
    }
    er_write = (er_write - 1) & er_mask;
    SF(base, er_buf + 4 * er_write, x);
    // tap 0, the two all-passes, the eight-tap line
    er_now[0] = (er_now[0] - 1) & er_mask;
    const float f = first.fade ? fade(first.now, first.old) : first.now;
    float v = LF(base, er_buf + 4 * er_now[0]) * f;
    if (er_fade[0]) {
      er_old[0] = (er_old[0] - 1) & er_mask;
      v = fade(v, LF(base, er_buf + 4 * er_old[0]) * f);
    }
    ap1_read = (ap1_read - 1) & ap1_mask;
    ap1_write = (ap1_write - 1) & ap1_mask;
    const float p1 = Fnms(ap_gain, v, LF(base, ap1_buf + 4 * ap1_read));
    SF(base, ap1_buf + 4 * ap1_write, Fma(ap_gain, p1, v));
    ap2_read = (ap2_read - 1) & ap2_mask;
    ap2_write = (ap2_write - 1) & ap2_mask;
    const float p2 = Fnms(ap_gain, p1, LF(base, ap2_buf + 4 * ap2_read));
    SF(base, ap2_buf + 4 * ap2_write, Fma(ap_gain, p2, p1));
    diffused[0] = p2;
    taps_pos[0] = (taps_pos[0] - 1) & taps_mask;
    SF(base, taps_buf + 4 * taps_pos[0], p2);
    for (uint32_t k = 1; k < 8; ++k) {
      taps_pos[k] = (taps_pos[k] - 1) & taps_mask;
      diffused[k] = LF(base, taps_buf + 4 * taps_pos[k]);
    }
    // the late lines
    const float a = early.fade ? fade(early.now, early.old) : early.now;
    for (int32_t j = 0; j < lines; ++j) {
      er_now[j + 1] = (er_now[j + 1] - 1) & er_mask;
      float tap = LF(base, er_buf + 4 * er_now[j + 1]);
      if (er_fade[j + 1]) {
        er_old[j + 1] = (er_old[j + 1] - 1) & er_mask;
        tap = fade(tap, LF(base, er_buf + 4 * er_old[j + 1]));
      }
      const float fed = a * tap;
      l_now[j] = (l_now[j] - 1) & l_mask[j];
      float y;
      if (l_fade[j]) {
        l_old[j] = (l_old[j] - 1) & l_mask[j];
        const float n = LF(base, l_buf[j] + 4 * l_now[j]) * g_now[j];
        const float o = LF(base, l_buf[j] + 4 * l_old[j]) * g_old[j];
        y = Fma(Fma(o, fade.fr, n * fade.fi), inv, fed);
      } else {
        y = Fma(LF(base, l_buf[j] + 4 * l_now[j]), g_now[j], fed);
      }
      y = e * y;
      if (c_fade[j]) {
        const float cj = fade(c_now[j], c_old[j]);
        st[j] = Fma(1.0f - cj, st[j], cj * y);
      } else {
        st[j] = Fma(c_now[j], y, c_rest[j] * st[j]);
      }
      l_write[j] = (l_write[j] - 1) & l_mask[j];
    }
    // the rotation of the eight states, back into the lines (line j takes element j + 1)
    {
      const float nd = -d;
      const float ka[4] = {d, nd, d, nd}, kb[4] = {d, d, nd, nd};
      const float32x4_t va = vld1q_f32(ka), vb = vld1q_f32(kb), vd = vdupq_n_f32(d), vn = vdupq_n_f32(nd);
      const float32x4_t lo = vld1q_f32(st), hi = vld1q_f32(st + 4);
      w0 = vfmaq_f32(lo, va, vrev64q_f32(lo));
      w2 = vfmaq_f32(w0, vb, vextq_f32(w0, w0, 2));
      w1 = vfmaq_f32(hi, va, vrev64q_f32(hi));
      w3 = vfmaq_f32(w1, vb, vextq_f32(w1, w1, 2));
      m_lo = vfmaq_f32(w2, vd, w3);
      m_hi = vfmaq_f32(w3, vn, w2);
      float m[8];
      vst1q_f32(m, m_lo);
      vst1q_f32(m + 4, m_hi);
      for (uint32_t j = 0; j < 8; ++j)
        SF(base, l_buf[j] + 4 * l_write[j], m[(j + 1) & 7]);
    }
    // the output channels
    uint32_t bit = 1;
    for (int32_t ch = 0; ch < channels; ++ch, bit = (bit << 1) | (bit >> 31)) {
      const uint32_t o = frame_out + uint32_t(ch) * 4;
      if ((speakers & bit) == 0) {
        SF(base, o, LF(base, o - out + in));
        continue;
      }
      float s0 = 0.0f, s1 = 0.0f, t0 = 0.0f, t1 = 0.0f, s2 = 0.0f, t2 = 0.0f;
      uint32_t k = uint32_t(ch), done = 0;
      if (per_channel >= 2) {
        do {
          const uint32_t k2 = k + uint32_t(channels);
          s0 = s0 + diffused[k];
          s1 = s1 + diffused[k2];
          l_out[k] = (l_out[k] - 1) & l_mask[k];
          t0 = LF(base, l_buf[k] + 4 * l_out[k]) + t0;
          l_out[k2] = (l_out[k2] - 1) & l_mask[k2];
          t1 = LF(base, l_buf[k2] + 4 * l_out[k2]) + t1;
          out_used[k] = out_used[k2] = true;
          done += 2;
          k += 2 * uint32_t(channels);
        } while (int32_t(done) < int32_t(per_channel) - 1);
      }
      if (done < per_channel) {
        s2 = diffused[k];
        l_out[k] = (l_out[k] - 1) & l_mask[k];
        t2 = LF(base, l_buf[k] + 4 * l_out[k]);
        out_used[k] = true;
      }
      s1 = s2 + s1;
      t1 = t2 + t1;
      s0 = s1 + s0;
      t1 = t1 + t0;
      if (dry == 0.0) {
        SF(base, o, s0 + t1);
      } else {
        const float wet = float(std::fma(double(LF(base, o - out + in)), dry, double(s0)));
        SF(base, o, wet + t1);
      }
    }
  }

  // 5. The state back into the object: what the original stores every sample, once.
  SF(base, self + 36, lp1);
  SF(base, self + 40, lp2);
  if (shelf) {
    SF(base, self + 68, z1);
    SF(base, self + 72, z2);
  }
  SU(base, self + 1220, er_write);
  SU(base, self + 1224, er_now[0]);
  if (er_fade[0])
    SU(base, self + 1260, er_old[0]);
  SU(base, self + 1436, ap1_write);
  SU(base, self + 1440, ap2_write);
  SU(base, self + 1444, ap1_read);
  SU(base, self + 1448, ap2_read);
  for (uint32_t k = 0; k < 8; ++k)
    SU(base, self + 1364 + 4 * k, taps_pos[k]);
  for (int32_t j = 0; j < lines; ++j) {
    SU(base, self + 1228 + 4 * uint32_t(j), er_now[j + 1]);
    if (er_fade[j + 1])
      SU(base, self + 1264 + 4 * uint32_t(j), er_old[j + 1]);
    SU(base, self + 988 + 4 * uint32_t(j), l_now[j]);
    if (l_fade[j])
      SU(base, self + 1020 + 4 * uint32_t(j), l_old[j]);
    SU(base, self + 956 + 4 * uint32_t(j), l_write[j]);
    SF(base, state + 4 * uint32_t(j), st[j]);
  }
  for (uint32_t k = 0; k < 8; ++k)
    if (out_used[k])
      SU(base, self + 1052 + 4 * k, l_out[k]);
  SV(base, work, w0);
  SV(base, work + 16, w1);
  SV(base, work + 32, w2);
  SV(base, work + 48, w3);
  SV(base, work + 64, m_lo);
  SV(base, work + 80, m_hi);
  return true;
}

}  // namespace tw2de::native_reverb

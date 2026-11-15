// tw2de (2026-10-24): the two filter loops of the game's audio mixer (its thread was 50-69% of a core in the
// v-perf profiles, these two a fifth of it) as plain native loops, the same arithmetic as the recompiled ones.
//
// Both are a state-variable filter (Chamberlin) over a block of samples: low, band and high from each input sample,
// the output one of them or the notch (low + high). The recompiled code keeps every PowerPC register in the context
// (memory): each operation is a load and a store of ctx, every compare stores four condition bytes, and the loop
// runs through them. Here the same operations run on locals, in the same order, with the same expressions (double
// fma rounded to float, as the generated code computes fmadds / fnmsubs / fsubs / fadds), so every stored value is
// the same; the state and output pointers are read and written in the original's order (the output may be the
// input). Every guest store marks its page's store card, as the generated code does (tools/store_cards.py). The red
// zone stores the originals make below r1 (the coefficients passed through the stack, the saved r29-r31 and lr) are
// not made: nothing reads below the stack pointer.
//
// At return the context holds what the originals leave: r3-r12, ctr, the CA bit, cr0 and cr6, f0 and f9-f13 (checked
// against the originals, whole context and memory, under qemu-aarch64 with the Switch compiler and flags:
// tools/native_audio_test/).
#pragma once

#include <cstdint>
#include <cstring>

namespace tw2de::native_audio {

inline float LoadF(const uint8_t *base, uint32_t ea) {
  uint32_t u;
  std::memcpy(&u, base + ea, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline uint32_t LoadU(const uint8_t *base, uint32_t ea) {
  uint32_t u;
  std::memcpy(&u, base + ea, 4);
  return __builtin_bswap32(u);
}
inline float ToS(double v);
inline void StoreF(uint8_t *base, uint32_t ea, double v) {
  const float f = ToS(v);  // stfs: fcvt (a signalling NaN loaded and stored back comes out quiet, as there)
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(base + ea, &u, 4);
  base[ea >> 12] = 0;  // the store card (REX_CARD)
}
// One instruction each, in the form and operand order the Switch build's objects of the originals have (GCC chose
// them from the generated expressions: some subtractions in single precision, the multiplicands of some fmadd the
// other way round); which NaN comes out where two meet depends on it (FPCR.DN is off). asm volatile: kept in
// program order, after the FPCR write of the flush mode.
inline double FsubD(double n, double m) { double d; asm volatile("fsub %d0, %d1, %d2" : "=w"(d) : "w"(n), "w"(m)); return d; }
inline double FaddD(double n, double m) { double d; asm volatile("fadd %d0, %d1, %d2" : "=w"(d) : "w"(n), "w"(m)); return d; }
inline float FsubS(float n, float m) { float d; asm volatile("fsub %s0, %s1, %s2" : "=w"(d) : "w"(n), "w"(m)); return d; }
inline float FnegS(float n) { float d; asm volatile("fneg %s0, %s1" : "=w"(d) : "w"(n)); return d; }
// n * m - a, fused (FNMSUB)
inline double Fnmsub(double n, double m, double a) { double d; asm volatile("fnmsub %d0, %d1, %d2, %d3" : "=w"(d) : "w"(n), "w"(m), "w"(a)); return d; }
// n * m + a, fused (FMADD)
inline double Fmadd(double n, double m, double a) { double d; asm volatile("fmadd %d0, %d1, %d2, %d3" : "=w"(d) : "w"(n), "w"(m), "w"(a)); return d; }
inline float ToS(double v) { float d; asm volatile("fcvt %s0, %d1" : "=w"(d) : "w"(v)); return d; }
inline double ToD(float v) { double d; asm volatile("fcvt %d0, %s1" : "=w"(d) : "w"(v)); return d; }

inline float AsFloat(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// sub_829C7260(r3 = the filter): one channel. [r3+0] input, [r3+4] output, [r3+8] low state, [r3+12] band state,
// [r3+16] samples, [r3+24] mode (0 low, 1 band, 2 high, else notch), [r3+28] f (float), [r3+32] q (float).
template <class Ctx> void FilterMono(Ctx &ctx, uint8_t *base) {
  const uint32_t s = ctx.r3.u32;
  const uint32_t f_bits = LoadU(base, s + 28), q_bits = LoadU(base, s + 32);
  const uint32_t low_p = LoadU(base, s + 8), band_p = LoadU(base, s + 12), n = LoadU(base, s + 16);
  ctx.fpscr.disableFlushMode();
  const double f11 = ToD(AsFloat(f_bits)), f10 = ToD(AsFloat(q_bits));
  const uint32_t in = LoadU(base, s + 0);
  uint32_t out = LoadU(base, s + 4);
  const int32_t mode = int32_t(LoadU(base, s + 24));
  const float low_raw = LoadF(base, low_p);
  double f13 = ToD(low_raw);
  double f0 = ToD(LoadF(base, band_p));
  double f9 = ctx.f9.f64, f12 = ctx.f12.f64;
  // the registers as the generated code computes them: 64-bit (subf, addi), addresses their low 32 bits
  uint64_t r9 = in, r10 = n;
  if (n != 0) {
    r9 = uint64_t(in) - uint64_t(out);
    for (uint32_t i = 0; i < n; ++i) {
      // fsubs f9 = x - low; fnmsubs f12 = f9 - q * band; fmadds band = f12 * f + band; fmadds low = band * f + low
      const double x = ToD(LoadF(base, uint32_t(r9 + out)));
      f9 = ToD(ToS(FsubD(x, f13)));
      const float t = ToS(Fnmsub(f10, f0, f9));
      f12 = ToD(FnegS(t));
      f0 = ToD(ToS(Fmadd(f12, f11, f0)));
      f13 = ToD(ToS(Fmadd(f11, f0, f13)));
      if (mode == 0) {
        StoreF(base, out, f13);
      } else if (mode == 2) {
        StoreF(base, out, f12);
      } else if (mode == 1) {
        StoreF(base, out, f0);
      } else {
        const float notch = FsubS(ToS(f13), t);  // fadds low + high, as low - t
        f12 = ToD(notch);
        StoreF(base, out, f12);
      }
      out += 4;
    }
    r10 = uint64_t(out) + 16;
    ctx.ctr.u64 = 0;
    // the last compare of the loop: cmpwi cr6, r8 (mode) against 0 when it stored low, else against 2 / 1
    if (mode == 0)
      ctx.cr6.template compare<int32_t>(mode, 0, ctx.xer);
    else if (mode == 2)
      ctx.cr6.template compare<int32_t>(mode, 2, ctx.xer);
    else
      ctx.cr6.template compare<int32_t>(mode, 1, ctx.xer);
  } else {
    ctx.cr6.template compare<uint32_t>(n, 0, ctx.xer);
  }
  if (n != 0) {
    StoreF(base, low_p, f13);
  } else {
    // the Switch object of the original stores the loaded single itself here (GCC folded the fcvt pair; a
    // signalling NaN is not quieted on this path, as it is for the band state)
    uint32_t u;
    std::memcpy(&u, &low_raw, 4);
    u = __builtin_bswap32(u);
    std::memcpy(base + low_p, &u, 4);
    base[low_p >> 12] = 0;
  }
  StoreF(base, band_p, f0);
  ctx.r6.u64 = band_p;
  ctx.r7.u64 = low_p;
  ctx.r8.u64 = uint64_t(mode < 0 ? uint32_t(mode) : uint32_t(mode));
  ctx.r9.u64 = r9;
  ctx.r10.u64 = r10;
  ctx.r11.u64 = out;
  ctx.f0.f64 = f0;
  ctx.f9.f64 = f9;
  ctx.f10.f64 = f10;
  ctx.f11.f64 = f11;
  ctx.f12.f64 = f12;
  ctx.f13.f64 = f13;
}

// sub_829C7178(r3 = the filter): interleaved channels. [r3+0] input, [r3+4] output, [r3+8] low states, [r3+12] band
// states (one per channel), [r3+16] samples in all, [r3+20] channels, [r3+24] mode, [r3+28] f, [r3+32] q.
template <class Ctx> void FilterInterleaved(Ctx &ctx, uint8_t *base) {
  ctx.r12.u64 = ctx.lr;
  const uint32_t s = ctx.r3.u32;
  const uint32_t f_bits = LoadU(base, s + 28), q_bits = LoadU(base, s + 32);
  const uint32_t total = LoadU(base, s + 16), channels = LoadU(base, s + 20);
  const uint32_t out0 = LoadU(base, s + 4);
  ctx.fpscr.disableFlushMode();
  const double f11 = ToD(AsFloat(f_bits)), f10 = ToD(AsFloat(q_bits));
  const uint32_t in = LoadU(base, s + 0);
  uint32_t frames = channels ? total / channels : 0;
  ctx.cr0.template compare<int32_t>(int32_t(frames), 0, ctx.xer);
  const uint32_t low = LoadU(base, s + 8), band = LoadU(base, s + 12);
  const int32_t mode = int32_t(LoadU(base, s + 24));
  uint64_t r3 = s, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = out0, r8 = in, r11 = out0;
  uint64_t r30 = ctx.r30.u64;
  double f0 = ctx.f0.f64, f9 = ctx.f9.f64, f12 = ctx.f12.f64, f13 = ctx.f13.f64;
  bool wrote_cr6 = false;
  int32_t cr6_rhs = 0;
  uint32_t cr6_u = 0;
  bool cr6_unsigned = false;
  if (frames != 0) {
    r30 = uint32_t(channels << 2);
    r3 = uint64_t(in) - uint64_t(out0);
    for (;;) {
      r11 = 32;
      cr6_unsigned = true;
      cr6_u = channels;
      wrote_cr6 = true;
      if (channels != 0) {
        const uint64_t d_in = r3 - uint64_t(low);
        ctx.ctr.u64 = channels;
        r11 = low;
        r4 = d_in + r6;
        r8 = uint64_t(band) - uint64_t(low);
        r5 = r6 - uint64_t(low);
        for (uint32_t c = 0; c < channels; ++c) {
          const float low_s = LoadF(base, r11);
          f12 = ToD(low_s);
          cr6_unsigned = false;
          cr6_rhs = 2;
          f0 = ToD(FsubS(LoadF(base, r4 + r11), low_s));
          f9 = ToD(LoadF(base, r8 + r11));
          f13 = ToD(FnegS(ToS(Fnmsub(f9, f10, f0))));
          f0 = ToD(ToS(Fmadd(f13, f11, f9)));
          StoreF(base, r8 + r11, f0);
          f12 = ToD(ToS(Fmadd(f0, f11, f12)));
          StoreF(base, r11, f12);
          if (mode == 2) {
            StoreF(base, r5 + r11, f13);
          } else {
            cr6_rhs = 0;
            if (mode == 0) {
              StoreF(base, r5 + r11, f12);
            } else {
              cr6_rhs = 1;
              if (mode != 1)
                f0 = ToD(ToS(FaddD(f12, f13)));
              StoreF(base, r5 + r11, f0);
            }
          }
          r11 += 4;
        }
        ctx.ctr.u64 = 0;
      }
      ctx.xer.ca = frames > 0;
      frames -= 1;
      ctx.cr0.template compare<int32_t>(int32_t(frames), 0, ctx.xer);
      r6 = r30 + r6;
      if (frames == 0)
        break;
    }
  }
  if (wrote_cr6) {
    if (cr6_unsigned)
      ctx.cr6.template compare<uint32_t>(cr6_u, 0, ctx.xer);
    else
      ctx.cr6.template compare<int32_t>(mode, cr6_rhs, ctx.xer);
  }
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = channels;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = low;
  ctx.r10.u64 = frames;
  ctx.r11.u64 = r11;
  (void)r30;  // r29-r31 are the caller's again at return (__restgprlr_29)
  ctx.f0.f64 = f0;
  ctx.f9.f64 = f9;
  ctx.f10.f64 = f10;
  ctx.f11.f64 = f11;
  ctx.f12.f64 = f12;
  ctx.f13.f64 = f13;
}

}  // namespace tw2de::native_audio

// tw2de (2026-10-23): the game's two hottest vector-math functions (XNA Math's matrix multiply and its bounding
// box transform) as native AArch64 code, bit for bit the Switch build's recompiled ones.
//
// What "bit for bit" takes. The generated code is the PowerPC sequence on the context's 128 vector registers: every
// result stored there and loaded back, each 16-byte load and store byte-swapped through a table. Here the same
// operations run on local registers. Every float operation is one instruction (inline asm), in the order and with the
// operands the Switch build's own object of the original has: GCC, compiling the original, put the two multiplicands
// of some fmul/fmla, and the two addends of some fadd, the other way round from the PowerPC source (it treats them as
// commutative), and that order is what decides which NaN comes out when two meet (FPCR.DN is off, payloads pass
// through). The order was read off the original's machine code (the notes' trace2.py) and the result checked against
// it under qemu (the notes' vtest): every output byte, r3..r11 and the FPCR, over ordinary and special inputs
// (denormals, infinities, quiet and signalling NaNs with payloads, two or more NaNs together, overflow, cancellation,
// outputs over the inputs, unaligned pointers), flush to zero on and off before the call.
//
// Lanes: a load is byte-reversed per 32-bit word (rev32), so guest element k is lane k (the generated code reverses
// all 16 bytes and keeps element k in lane 3 - k; every operation here is lane by lane, so only the splat lane
// changes). Loads and stores are masked to 16 bytes as lvx / stvx are. All loads are done before any store, as in the
// originals (the output may be one of the inputs).
//
// Besides memory the originals leave r9, r10, r11 (set here too) and vector registers (not set: no call site reads one
// of them after the call; all 669 call sites checked, all plain bl). The flush mode is turned on where the originals
// do, before the first float operation.
#pragma once

#if defined(__aarch64__)

#include <arm_neon.h>

#include <cstdint>

namespace tw2de::native_vmath {

using V = float32x4_t;

inline V Load(const uint8_t *base, uint32_t ea) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(base + (ea & ~0xFu))));
}
inline void Store(uint8_t *base, uint32_t ea, V v) {
  vst1q_u8(base + (ea & ~0xFu), vrev32q_u8(vreinterpretq_u8_f32(v)));
  // tw2de (2026-10-24): the store card of the block's page, as the generated stvx does (tools/store_cards.py; the
  // cards are the guest window's first MB, at base).
  base[(ea & ~0xFu) >> 12] = 0;
}
template <int k> inline V Splat(V v) { return vdupq_laneq_f32(v, k); }

// One instruction each, operands in the order given (asm volatile: kept in program order, after the FPCR write).
inline V Fmul(V n, V m) { V d; asm volatile("fmul %0.4s, %1.4s, %2.4s" : "=w"(d) : "w"(n), "w"(m)); return d; }
inline V Fadd(V n, V m) { V d; asm volatile("fadd %0.4s, %1.4s, %2.4s" : "=w"(d) : "w"(n), "w"(m)); return d; }
inline V Fsub(V n, V m) { V d; asm volatile("fsub %0.4s, %1.4s, %2.4s" : "=w"(d) : "w"(n), "w"(m)); return d; }
inline V Fmin(V n, V m) { V d; asm volatile("fmin %0.4s, %1.4s, %2.4s" : "=w"(d) : "w"(n), "w"(m)); return d; }
inline V Fmax(V n, V m) { V d; asm volatile("fmax %0.4s, %1.4s, %2.4s" : "=w"(d) : "w"(n), "w"(m)); return d; }
// acc + n * m, fused (FMLA)
inline V Fmla(V acc, V n, V m) { asm volatile("fmla %0.4s, %1.4s, %2.4s" : "+w"(acc) : "w"(n), "w"(m)); return acc; }

// sub_822D09F8(out r3, b r4, a r5): out[i] = a[i].x * b0 + a[i].y * b1 + a[i].z * b2 + a[i].w * b3.
template <class Ctx> void MatrixMultiply(Ctx &ctx, uint8_t *base) {
  const uint32_t out = ctx.r3.u32, b = ctx.r4.u32, a = ctx.r5.u32;
  const V a0 = Load(base, a), a1 = Load(base, a + 16), a2 = Load(base, a + 32), a3 = Load(base, a + 48);
  const V b0 = Load(base, b), b1 = Load(base, b + 16), b2 = Load(base, b + 32), b3 = Load(base, b + 48);
  ctx.fpscr.enableFlushMode();
  // the original's trace, t1..t16
  const V t1 = Fmul(b0, Splat<0>(a0));
  const V t2 = Fmul(b0, Splat<0>(a1));
  const V t3 = Fmul(b0, Splat<0>(a2));
  const V t4 = Fmul(b0, Splat<0>(a3));
  const V t5 = Fmla(t1, Splat<1>(a0), b1);
  const V t6 = Fmla(t2, Splat<1>(a1), b1);
  const V t7 = Fmla(t3, Splat<1>(a2), b1);
  const V t8 = Fmla(t4, Splat<1>(a3), b1);
  const V t9 = Fmla(t8, b2, Splat<2>(a3));
  const V t10 = Fmla(t9, b3, Splat<3>(a3));
  const V t11 = Fmla(t6, Splat<2>(a1), b2);
  const V t12 = Fmla(t5, Splat<2>(a0), b2);
  const V t13 = Fmla(t7, Splat<2>(a2), b2);
  const V t14 = Fmla(t12, Splat<3>(a0), b3);
  const V t15 = Fmla(t11, Splat<3>(a1), b3);
  const V t16 = Fmla(t13, b3, Splat<3>(a2));
  Store(base, out, t14);
  Store(base, out + 16, t15);
  Store(base, out + 32, t16);
  Store(base, out + 48, t10);
  ctx.r9.s64 = 16;
  ctx.r10.s64 = 32;
  ctx.r11.s64 = 48;
}

// sub_82395A10(out r3, box r4, m r5): the box (min at r4, max at r4 + 16) through the matrix m (rows m0..m3 at r5):
// out = the minimum of its 8 transformed corners, out + 16 the maximum.
template <class Ctx> void TransformBox(Ctx &ctx, uint8_t *base) {
  const uint32_t out = ctx.r3.u32, box = ctx.r4.u32, m = ctx.r5.u32;
  const V mn = Load(base, box), mx = Load(base, box + 16);
  const V m0 = Load(base, m), m1 = Load(base, m + 16), m2 = Load(base, m + 32), m3 = Load(base, m + 48);
  ctx.fpscr.enableFlushMode();
  // the original's trace, t1..t28
  const V t1 = Fmla(m3, m0, Splat<0>(mn));
  const V t2 = Fsub(mx, mn);
  const V t3 = Fmla(t1, Splat<2>(mn), m2);
  const V t4 = Fmul(m0, Splat<0>(t2));
  const V t5 = Fmul(m2, Splat<2>(t2));
  const V t6 = Fmul(m1, Splat<1>(t2));
  const V t7 = Fmla(t3, Splat<1>(mn), m1);  // the first corner
  const V t8 = Fadd(t4, t7);
  const V t9 = Fadd(t6, t7);
  const V t10 = Fadd(t5, t7);
  const V t11 = Fadd(t8, t6);
  const V t12 = Fadd(t8, t5);
  const V t13 = Fadd(t9, t5);
  const V t14 = Fadd(t5, t11);
  const V t15 = Fmin(t7, t8);
  const V t16 = Fmax(t7, t8);
  const V t17 = Fmin(t15, t9);
  const V t18 = Fmax(t16, t9);
  const V t19 = Fmin(t17, t11);
  const V t20 = Fmax(t18, t11);
  const V t21 = Fmin(t19, t10);
  const V t22 = Fmax(t20, t10);
  const V t23 = Fmin(t21, t12);
  const V t24 = Fmax(t22, t12);
  const V t25 = Fmin(t23, t13);
  const V t26 = Fmax(t24, t13);
  const V t27 = Fmin(t25, t14);
  const V t28 = Fmax(t26, t14);
  Store(base, out, t27);
  Store(base, out + 16, t28);
  ctx.r9.s64 = 32;
  ctx.r10.s64 = 48;
  ctx.r11.s64 = 16;
}

}  // namespace tw2de::native_vmath

#endif  // __aarch64__

/**
 * @file        vmx_fp.h
 * @brief       VMX floating-point instructions with PowerPC semantics on AArch64
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Each helper has an AArch64 path that gives the PowerPC/AltiVec result, and an `#else` path that is exactly the
 * expression the code generator emitted before (so x86 builds are unchanged). The AArch64 results were compared bit
 * for bit with the instructions themselves under qemu-ppc -cpu 7450 (AltiVec), except vexptefp's out-of-range
 * inputs, where QEMU is wrong and the AltiVec definition is used (2^x: +INF for +INF/overflow, +0 for -INF/underflow).
 */

#pragma once

#include <rex/ppc/estimates.h>  // tw2de (2026-11-01)

#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_VMX_FP_PORTABLE)
#include <arm_neon.h>
#define REX_VMX_FP_NEON 1  // tw2de (2026-11-01): REX_VMX_FP_PORTABLE, the portable forms (diagnostic)
#endif

namespace rex::ppc {

#if defined(REX_VMX_FP_NEON)
inline float32x4_t vmx_to_neon(simde__m128 a) {
  float32x4_t r;
  std::memcpy(&r, &a, sizeof(r));
  return r;
}
inline simde__m128 vmx_from_neon(float32x4_t a) {
  simde__m128 r;
  std::memcpy(&r, &a, sizeof(r));
  return r;
}
inline simde__m128 vmx_from_neon(uint32x4_t a) {
  simde__m128 r;
  std::memcpy(&r, &a, sizeof(r));
  return r;
}
#endif

// vmaxfp / vminfp: a NaN in either operand gives a NaN; max(+0, -0) = +0, min(+0, -0) = -0.
// simde_mm_max_ps / simde_mm_min_ps (without SIMDE_FAST_NANS) implement the x86 rule on every host: a > b ? a : b,
// i.e. the second operand when either is NaN (and when comparing +0 with -0). AArch64 FMAX / FMIN have the PowerPC rule.
inline simde__m128 vmx_max_ps(simde__m128 a, simde__m128 b) {
#if defined(REX_VMX_FP_NEON)
  return vmx_from_neon(vmaxq_f32(vmx_to_neon(a), vmx_to_neon(b)));
#else
  return simde_mm_max_ps(a, b);
#endif
}

inline simde__m128 vmx_min_ps(simde__m128 a, simde__m128 b) {
#if defined(REX_VMX_FP_NEON)
  return vmx_from_neon(vminq_f32(vmx_to_neon(a), vmx_to_neon(b)));
#else
  return simde_mm_min_ps(a, b);
#endif
}

// vmaddfp: a * b + c with a single rounding (AltiVec fuses it). The generated code was mul_ps then add_ps: two
// roundings (1 ulp off for about 1 in 5 random inputs) and an overflow of the intermediate product (FLT_MAX * 2 -
// FLT_MAX gave +INF, PowerPC gives FLT_MAX). AArch64 FMLA is the fused operation, with the same NaN choice as
// PowerPC (the addend's NaN first).
inline simde__m128 vmx_madd_ps(simde__m128 a, simde__m128 b, simde__m128 c) {
#if defined(REX_VMX_FP_NEON)
  return vmx_from_neon(vfmaq_f32(vmx_to_neon(c), vmx_to_neon(a), vmx_to_neon(b)));
#else
  return simde_mm_add_ps(simde_mm_mul_ps(a, b), c);
#endif
}

// vnmsubfp: -(a * b - c), fused, and a NaN result is not negated. The generated code was two roundings and an XOR of
// the sign bit, which also flipped the sign of NaNs. c - a * b (FMLS) is not the same: an exact cancellation gives +0
// where PowerPC gives -0. So: the fused a * b - c, then negate the lanes that are not NaN. c is negated only where it
// is not NaN, so that a NaN coming from c keeps its sign.
inline simde__m128 vmx_nmsub_ps(simde__m128 a, simde__m128 b, simde__m128 c) {
#if defined(REX_VMX_FP_NEON)
  const float32x4_t vc = vmx_to_neon(c);
  const float32x4_t nc = vbslq_f32(vceqq_f32(vc, vc), vnegq_f32(vc), vc);
  const float32x4_t t = vfmaq_f32(nc, vmx_to_neon(a), vmx_to_neon(b));
  return vmx_from_neon(vbslq_f32(vceqq_f32(t, t), vnegq_f32(t), t));
#else
  return simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(a, b), c),
                         simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))));
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// The estimate instructions (vrefp, vrsqrtefp, vexptefp, vlogefp).
//
// tw2de (2026-10-24): every estimate here comes from xenia-edge (BSD 3-Clause, https://github.com/has207/xenia-edge,
// commit b3e8fda2):
//  - vrsqrtefp: its hardware-matched table (kVRsqrteCoefficients, src/xenia/cpu/backend/vrsqrte_table.cc), the same
//    results as the table used before for every normal input (4,161,536 cases checked);
//  - vrefp: its exact reciprocal (OPCODE_RECIP, a64_sequences.cc), denormal inputs and outputs flushed;
//  - vexptefp and vlogefp: its polynomials (OPCODE_POW2 / OPCODE_LOG2, a64_sequences.cc, coefficients in
//    a64_backend.cc), snapped onto the 2^-11 grid the hardware's estimates lie on.
// tw2de (2026-11-01): vrefp, vexptefp and vlogefp now come from the 360's own tables (rex/ppc/estimates.h,
// read from a retail console by Xenia); the xenia-edge forms for them below are no longer used.
// vrsqrtefp stays bit for bit the hardware's on the results Xenia's test suite recorded; vrefp, vexptefp and vlogefp are
// close to them but not always the same bits (sdk/tests/vmx_estimates_test.cpp counts the differences). The vector unit
// is taken to be in non-Java mode (NJ = 1: denormal inputs and outputs are zero), which is what those hardware results
// show and what the generated code runs in (FPCR.FZ); the float arithmetic assumes round to nearest, as VMX always is.
//
// Elsewhere (x86) the previous expressions are kept: the exact reciprocal / reciprocal square root, a polynomial for
// 2^x and log2f.
// ---------------------------------------------------------------------------------------------------------------------

namespace vmx_est {
// vrsqrtefp: 32 segments by (exponent parity, mantissa bits 22..19): base, slope, and an 8-bit pattern that takes one
// more ulp off for some values of the low 3 bits of the interpolation key (mantissa bits 18..9).
// Source: xenia-edge (BSD 3-Clause, https://github.com/has207/xenia-edge, commit b3e8fda2), kVRsqrteCoefficients and
// ComputeNormalVRsqrteTableValue in src/xenia/cpu/backend/vrsqrte_table.cc (each coefficient a slope in the high 16 bits
// and a base in the low 16). The three tables below are computed from those at compile time, in the form the NEON code
// uses; every normal input (all exponents and mantissa keys, 4,161,536 cases) gives xenia-edge's result.
inline constexpr uint32_t kVRsqrteCoefficients[32] = {
    0x0568B4FD, 0x04F3AF97, 0x048DAAA5, 0x0435A618, 0x03E7A1E4, 0x03A29DFE, 0x03659A5C, 0x032E96F8,
    0x02FC93CA, 0x02D090CE, 0x02A88DFE, 0x02838B57, 0x026188D4, 0x02438673, 0x02268431, 0x020B820B,
    0x03D27FFA, 0x03807C29, 0x033878AA, 0x02F97572, 0x02C27279, 0x02926FB7, 0x02666D26, 0x023F6AC0,
    0x021D6881, 0x01FD6665, 0x01E16468, 0x01C76287, 0x01AF60C1, 0x01995F12, 0x01855D79, 0x01735BF4};

// xenia-edge's interpolation for a positive normal input, as there: the result's bits with the canonical exponent of
// the input's parity (0x3F800000 plus the adjustment).
constexpr uint32_t XeniaVRsqrte(uint32_t input) {
  const uint32_t mantissa = input & 0x7FFFFF;
  const uint32_t coefficient = kVRsqrteCoefficients[(((input >> 23) & 1) << 4) | (mantissa >> 19)];
  uint32_t estimate = ((coefficient << 10) & 0x3FFFC00) - (((mantissa >> 9) & 1023) * (coefficient >> 16));
  int32_t adjustment = 0;
  if (!(estimate & 0x02000000)) {
    const uint32_t normalized = estimate & 0x1FFFFFF;
    uint32_t leading = 0;
    for (uint32_t bit = 0x80000000u; !(normalized & bit); bit >>= 1)
      ++leading;
    adjustment += 6 - int32_t(leading);
    estimate <<= leading - 6;
  }
  if ((estimate & 5) && (estimate & 2))
    estimate += 4;
  return (0x3F800000u + uint32_t(adjustment) * 0x00800000u) | ((estimate >> 2) & 0x7FFFFF);
}

struct RsqrtTables {
  uint32_t base[32], slope[32], round[32];
};
constexpr RsqrtTables MakeRsqrtTables() {
  RsqrtTables t{};
  for (uint32_t seg = 0; seg < 32; ++seg) {
    const uint32_t c = kVRsqrteCoefficients[seg], b = c & 0xFFFF, a = c >> 16;
    // An even biased exponent (x in [0.5, 1) times a power of 4) gives 1/sqrt in (1, sqrt 2]: base b - 1 and the slope
    // as it is; an odd one gives (1/sqrt 2, 1], normalised: twice the base, less 1, and twice the slope.
    t.base[seg] = seg < 16 ? (b - 0x8000) << 8 : (2 * b - 0x8000) << 8;
    t.slope[seg] = seg < 16 ? a : 2 * a;
    for (uint32_t k = 0; k < 8; ++k) {
      const uint32_t input = (seg < 16 ? 0x3F000000u : 0x3F800000u) | ((seg & 15) << 19) | (k << 9);
      const uint32_t plain = (t.base[seg] - ((t.slope[seg] * k) >> 2)) & 0x7FFFFF;
      if ((XeniaVRsqrte(input) & 0x7FFFFF) != plain)
        t.round[seg] |= 1u << k;
    }
  }
  return t;
}
inline constexpr RsqrtTables kRsqrtTables = MakeRsqrtTables();
inline constexpr const uint32_t (&kRsqrtBase)[32] = kRsqrtTables.base;
inline constexpr const uint32_t (&kRsqrtSlope)[32] = kRsqrtTables.slope;
inline constexpr const uint32_t (&kRsqrtRound)[32] = kRsqrtTables.round;

// vexptefp / vlogefp: xenia-edge's minimax polynomials (a64_backend.cc est_consts): 2^f on [0, 1) and log2(1 + u) on
// [0, 1], evaluated by Horner with fused multiply-adds as its POW2_V128 / LOG2_V128 emit them.
inline constexpr float kExp2Poly[6] = {0.9999999266823865f,  0.6931530239113992f,   0.24015381838022493f,
                                       0.055826172900559086f, 0.008989127362479102f, 0.0018777841277241077f};
inline constexpr float kLog2Poly[7] = {1.8456866772102942e-06f, 1.4424953159391898f,  -0.7177910762015521f,
                                       0.4565216600899004f,     -0.2765407398023532f, 0.12100223739860312f,
                                       -0.025691088797142478f};

inline float f32(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t u32(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

template <size_t N>
inline float horner(const float (&c)[N], float v) {
  float acc = c[N - 1];
  for (size_t k = N - 1; k-- > 0;)
    acc = std::fma(acc, v, c[k]);
  return acc;
}

// Onto the estimates' 2^-11 grid (round to nearest even), as xenia-edge's EmitEstGridSnap: it keeps 2^0 = 1 and
// log2(2^n) = n exact.
inline float snap(float r) { return __builtin_roundevenf(r * 2048.0f) * (1.0f / 2048.0f); }

// One lane of vlogefp (bits in, bits out).
inline uint32_t loge(uint32_t x) {
  const uint32_t exp = (x >> 23) & 0xFF, man = x & 0x7FFFFF;
  if (exp == 0xFF && man) return x | 0x400000;          // NaN: quieted
  if (exp == 0) return 0xFF800000u;                     // +-0 and (flushed) denormals: -INF
  if (x >> 31) return 0x7FC00000u;                      // negative (-INF included): NaN
  if (exp == 0xFF) return x;                            // log2(+INF) = +INF
  const float e = float(int32_t(exp) - 127);
  const float u = f32(man | 0x3F800000u) - 1.0f;        // mantissa - 1, in [0, 1)
  return u32(snap(horner(kLog2Poly, u) + e));
}

// One lane of vexptefp.
inline uint32_t expte(uint32_t x) {
  const uint32_t exp = (x >> 23) & 0xFF, man = x & 0x7FFFFF;
  if (exp == 0xFF && man) return x | 0x400000;          // NaN: quieted
  const float v = exp == 0 ? 0.0f : f32(x);             // a denormal is flushed: 2^0
  if (v >= 128.0f) return 0x7F800000u;                  // overflow, +INF: +INF
  if (v < -126.0f) return 0u;                           // underflow, -INF: +0
  // 2^x = 2^floor(x) * 2^frac(x): the second factor in [1, 2), onto the grid.
  const float n = std::floor(v);
  const float scale = f32(uint32_t(int32_t(n) + 127) << 23);
  return u32(snap(horner(kExp2Poly, v - n)) * scale);
}
}  // namespace vmx_est

// vrefp: estimate of 1/x.
inline simde__m128 vmx_re_ps(simde__m128 x) {
  // tw2de (2026-11-01): the 360's own estimate, bit for bit (rex/ppc/estimates.h: Xenia's tables, read from a
  // retail console), on every host.
  uint32_t l[4];
  std::memcpy(l, &x, 16);
  for (uint32_t& e : l) e = hw_est::EstimateReciprocal(e);
  simde__m128 out;
  std::memcpy(&out, l, 16);
  return out;
}

// vrsqrtefp: estimate of 1/sqrt(x).
inline simde__m128 vmx_rsqrte_ps(simde__m128 x) {
#if defined(REX_VMX_FP_NEON)
  uint32x4_t v;
  std::memcpy(&v, &x, 16);
  const uint32x4_t sign = vandq_u32(v, vdupq_n_u32(0x80000000u));
  const uint32x4_t exp = vandq_u32(vshrq_n_u32(v, 23), vdupq_n_u32(0xFF));
  const uint32x4_t man = vandq_u32(v, vdupq_n_u32(0x7FFFFF));
  // Segment: exponent parity (an odd biased exponent is an even power of 2) and the top 4 mantissa bits.
  const uint32x4_t seg = vorrq_u32(vshlq_n_u32(vandq_u32(exp, vdupq_n_u32(1)), 4), vshrq_n_u32(man, 19));
  uint32x4_t base = vdupq_n_u32(0), slope = vdupq_n_u32(0), round = vdupq_n_u32(0);
#define REX_VMX_RSQRT_LANE(i)                                                                   \
  base = vld1q_lane_u32(&vmx_est::kRsqrtBase[vgetq_lane_u32(seg, i)], base, i);                 \
  slope = vld1q_lane_u32(&vmx_est::kRsqrtSlope[vgetq_lane_u32(seg, i)], slope, i);              \
  round = vld1q_lane_u32(&vmx_est::kRsqrtRound[vgetq_lane_u32(seg, i)], round, i);
  REX_VMX_RSQRT_LANE(0) REX_VMX_RSQRT_LANE(1) REX_VMX_RSQRT_LANE(2) REX_VMX_RSQRT_LANE(3)
#undef REX_VMX_RSQRT_LANE
  const uint32x4_t key = vandq_u32(vshrq_n_u32(man, 9), vdupq_n_u32(0x3FF));
  const uint32x4_t extra = vandq_u32(
      vshlq_u32(round, vnegq_s32(vreinterpretq_s32_u32(vandq_u32(key, vdupq_n_u32(7))))), vdupq_n_u32(1));
  const uint32x4_t rman = vsubq_u32(vsubq_u32(base, vshrq_n_u32(vmulq_u32(slope, key), 2)), extra);
  // Biased exponent of the result: (380 - e) / 2, rounded down.
  uint32x4_t r = vorrq_u32(vshlq_n_u32(vshrq_n_u32(vsubq_u32(vdupq_n_u32(380), exp), 1), 23), rman);
  r = vbslq_u32(vtstq_u32(v, vdupq_n_u32(0x80000000u)), vdupq_n_u32(0x7FC00000u), r);  // negative: NaN
  r = vbslq_u32(vceqzq_u32(exp), vorrq_u32(sign, vdupq_n_u32(0x7F800000u)), r);       // +-0 / denormal: +-INF
  const uint32x4_t special = vceqq_u32(exp, vdupq_n_u32(0xFF));
  const uint32x4_t inf = vbslq_u32(vtstq_u32(v, vdupq_n_u32(0x80000000u)), vdupq_n_u32(0x7FC00000u), vdupq_n_u32(0));
  const uint32x4_t inf_or_nan = vbslq_u32(vceqzq_u32(man), inf, vorrq_u32(v, vdupq_n_u32(0x400000)));
  r = vbslq_u32(special, inf_or_nan, r);
  simde__m128 out;
  std::memcpy(&out, &r, 16);
  return out;
#else
  return simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(x));
#endif
}

// vexptefp: estimate of 2^x.
inline simde__m128 vmx_expte_ps(simde__m128 x) {
  // tw2de (2026-11-01): the 360's own estimate, bit for bit (rex/ppc/estimates.h: Xenia's tables, read from a
  // retail console), on every host.
  uint32_t l[4];
  std::memcpy(l, &x, 16);
  for (uint32_t& e : l) e = hw_est::EstimatePow2(e);
  simde__m128 out;
  std::memcpy(&out, l, 16);
  return out;
}

// vlogefp: estimate of log2(x).
inline simde__m128 vmx_loge_ps(simde__m128 x) {
  // tw2de (2026-11-01): the 360's own estimate, bit for bit (rex/ppc/estimates.h: Xenia's tables, read from a
  // retail console), on every host.
  uint32_t l[4];
  std::memcpy(l, &x, 16);
  for (uint32_t& e : l) e = hw_est::EstimateLog2(e);
  simde__m128 out;
  std::memcpy(&out, l, 16);
  return out;
}

// vcmpbfp: bit 31 = !(a <= b), bit 30 = !(a >= -b). With a NaN in either operand both comparisons are false, so both
// bits are set (PowerPC: 0xC0000000). The generated code tested a > b and a < -b instead, which gives 0 for NaN.
inline simde__m128 vmx_cmpb_ps(simde__m128 a, simde__m128 b) {
#if defined(REX_VMX_FP_NEON)
  const float32x4_t va = vmx_to_neon(a), vb = vmx_to_neon(b);
  const uint32x4_t le = vcleq_f32(va, vb);
  const uint32x4_t ge = vcgeq_f32(va, vnegq_f32(vb));
  return vmx_from_neon(vorrq_u32(vbicq_u32(vdupq_n_u32(0x80000000u), le), vbicq_u32(vdupq_n_u32(0x40000000u), ge)));
#else
  const simde__m128 gt = simde_mm_and_ps(simde_mm_cmpgt_ps(a, b),
                                         simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))));
  const simde__m128 lt = simde_mm_and_ps(
      simde_mm_cmplt_ps(a, simde_mm_xor_ps(b, simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))))),
      simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x40000000))));
  return simde_mm_or_ps(gt, lt);
#endif
}


// vmsum3fp128 / vmsum4fp128: the dot product of x y z (w), splat to the four lanes. tw2de (2026-11-01): as Xenia's
// x64 backend in xenia-canary pull request #1342 (BSD), which matches the console: float64 products, summed as
// (x + z) + y or (x + z) + (y + w), then rounded TOWARD ZERO to single precision (the console's sums are truncated:
// 1.0 * 2.0 + 1.5 * 3.75 + 1.1 * 2.31 gives 4122A7EF there, not 4122A7F0). A result in the denormal range is a zero
// of its sign (the vector unit flushes), a float32 overflow of a finite sum is QNaN 0x7FC00000, and an infinity or NaN
// that came from the inputs stays. Denormal inputs are zero (the vector code runs with flush on: FPCR.FZ / MXCSR DAZ,
// which the float64 conversions follow). The generated code had a float32 dot product (rounded to nearest at each
// step) and the float64 sum only for its overflow case. Host lanes 3..0 hold x, y, z, w.
inline float vmx_dot_sum(double s) {
  uint64_t b;
  std::memcpy(&b, &s, 8);
  const uint32_t e = uint32_t(b >> 52) & 0x7FF;
  if (e == 0x7FF)  // from an infinity or NaN input
    return float(s);
  b &= 0xFFFFFFFFE0000000ull;  // the bits below single precision: toward zero
  if (e < 1023 - 126)          // below 2^-126 (or zero): a zero of its sign
    b &= 0x8000000000000000ull;
  else if (e > 1023 + 127) {   // 2^128 and up: overflow
    const uint32_t q = 0x7FC00000u;
    float f;
    std::memcpy(&f, &q, 4);
    return f;
  }
  double t;
  std::memcpy(&t, &b, 8);
  return float(t);  // exact
}

inline simde__m128 vmx_dot3_ps(simde__m128 a, simde__m128 b) {
  alignas(16) float fa[4], fb[4];
  simde_mm_store_ps(fa, a);
  simde_mm_store_ps(fb, b);
  const double s = (double(fa[3]) * fb[3] + double(fa[1]) * fb[1]) + double(fa[2]) * fb[2];
  return simde_mm_set1_ps(vmx_dot_sum(s));
}

inline simde__m128 vmx_dot4_ps(simde__m128 a, simde__m128 b) {
  alignas(16) float fa[4], fb[4];
  simde_mm_store_ps(fa, a);
  simde_mm_store_ps(fb, b);
  const double s =
      (double(fa[3]) * fb[3] + double(fa[1]) * fb[1]) + (double(fa[2]) * fb[2] + double(fa[0]) * fb[0]);
  return simde_mm_set1_ps(vmx_dot_sum(s));
}


// exact_fp (codegen option, tw2de 2026-11-01): the NaN the 360's vector unit gives. A lane whose result is NaN gets
// the first NaN operand of that lane, made quiet, in the order vA, vB, vC (vB the addend of the multiply-adds), or
// QNaN 0x7FC00000 when no operand was NaN (an invalid operation). The host picks a signaling NaN first, and AArch64's
// FMAX / FMIN its own way. Checked against Xenia's hardware-recorded tests: vaddfp, vsubfp, vmaxfp, vminfp (every
// lane) and vmaddfp / vnmsubfp (every NaN lane; their other lanes are the fused result, rounded to nearest, as
// before). The common case is one compare of the result.
inline simde__m128 vmx_exact_nan(simde__m128 r, simde__m128 a, simde__m128 b, simde__m128 c, int n) {
  if (!simde_mm_movemask_ps(simde_mm_cmpunord_ps(r, r))) [[likely]]
    return r;
  alignas(16) uint32_t lr[4], la[4], lb[4], lc[4];
  simde_mm_store_ps(reinterpret_cast<float*>(lr), r);
  simde_mm_store_ps(reinterpret_cast<float*>(la), a);
  simde_mm_store_ps(reinterpret_cast<float*>(lb), b);
  simde_mm_store_ps(reinterpret_cast<float*>(lc), c);
  auto is_nan = [](uint32_t v) { return (v & 0x7FFFFFFFu) > 0x7F800000u; };
  for (int i = 0; i < 4; ++i) {
    if (!is_nan(lr[i]))
      continue;
    lr[i] = is_nan(la[i]) ? (la[i] | 0x400000u)
          : is_nan(lb[i]) ? (lb[i] | 0x400000u)
          : (n == 3 && is_nan(lc[i])) ? (lc[i] | 0x400000u)
                                      : 0x7FC00000u;
  }
  return simde_mm_load_ps(reinterpret_cast<const float*>(lr));
}

// exact_fp_log (tw2de 2026-11-01): the host's vector result h; a lane whose NaN the 360 gives differently is
// reported once per instruction (rex::ppc::exact::report, the lane's bits in the low word, the lane in bits 32-33).
namespace exact {
[[gnu::cold, gnu::noinline]] void report(uint32_t insn, uint32_t function, const char* op, uint64_t host,
                                         uint64_t x360);  // rex/ppc/fpu_exact.h
[[gnu::cold, gnu::noinline]] void report_origin(uint32_t insn, uint32_t function, const char* op, uint64_t result,
                                                uint64_t a, uint64_t b, uint64_t c, uint32_t lr, uint32_t sp,
                                                const uint8_t* base);
}  // namespace exact
[[gnu::cold, gnu::noinline]] inline void vmx_check_slow(uint32_t insn, uint32_t function, const char* op,
                                                        simde__m128 h, simde__m128 a, simde__m128 b, simde__m128 c,
                                                        int n) {
  alignas(16) uint32_t lh[4], le[4];
  simde_mm_store_ps(reinterpret_cast<float*>(lh), h);
  simde_mm_store_ps(reinterpret_cast<float*>(le), vmx_exact_nan(h, a, b, c, n));
  for (int i = 0; i < 4; ++i)
    if (lh[i] != le[i]) {
      exact::report(insn, function, op, (uint64_t(i) << 32) | lh[i], (uint64_t(i) << 32) | le[i]);
      return;
    }
}
inline void vmx_check(uint32_t insn, uint32_t function, const char* op, simde__m128 h, simde__m128 a,
                      simde__m128 b, simde__m128 c, int n) {
  if (simde_mm_movemask_ps(simde_mm_cmpunord_ps(h, h))) [[unlikely]]
    vmx_check_slow(insn, function, op, h, a, b, c, n);
}

// fp_origin_log (tw2de 2026-11-01): a lane whose result is not finite while that lane of every operand is (cross:
// the dot products, the host lanes of the mask in both operands). Reported once per instruction: the lane in bits
// 32-33 of the result and the operands' bits in that lane (cross: the first lane the dot product uses).
[[gnu::cold, gnu::noinline]] inline void vmx_origin_slow(uint32_t insn, uint32_t function, const char* op,
                                                         simde__m128 h, simde__m128 a, simde__m128 b, simde__m128 c,
                                                         int cross, uint32_t lr, uint32_t sp, const uint8_t* base) {
  alignas(16) uint32_t lh[4], la[4], lb[4], lc[4];
  simde_mm_store_ps(reinterpret_cast<float*>(lh), h);
  simde_mm_store_ps(reinterpret_cast<float*>(la), a);
  simde_mm_store_ps(reinterpret_cast<float*>(lb), b);
  simde_mm_store_ps(reinterpret_cast<float*>(lc), c);
  auto fin = [](uint32_t v) { return (v & 0x7F800000u) != 0x7F800000u; };
  if (cross) {
    for (int i = 0; i < 4; ++i)
      if ((cross >> i) & 1 && (!fin(la[i]) || !fin(lb[i])))
        return;
    const int l = (cross & 1) ? 0 : 1;  // the first lane the product uses
    exact::report_origin(insn, function, op, lh[l], la[l], lb[l], lb[l], lr, sp, base);
    return;
  }
  for (int i = 0; i < 4; ++i)
    if (!fin(lh[i]) && fin(la[i]) && fin(lb[i]) && fin(lc[i])) {
      exact::report_origin(insn, function, op, (uint64_t(i) << 32) | lh[i], la[i], lb[i], lc[i], lr, sp, base);
      return;
    }
}
inline void vmx_origin(uint32_t insn, uint32_t function, const char* op, simde__m128 h, simde__m128 a, simde__m128 b,
                       simde__m128 c, int cross, uint32_t lr, uint32_t sp, const uint8_t* base) {
  const simde__m128i e = simde_mm_set1_epi32(0x7F800000);
  const simde__m128i x = simde_mm_and_si128(simde_mm_castps_si128(h), e);
  if (simde_mm_movemask_ps(simde_mm_castsi128_ps(simde_mm_cmpeq_epi32(x, e)))) [[unlikely]]
    vmx_origin_slow(insn, function, op, h, a, b, c, cross, lr, sp, base);
}

}  // namespace rex::ppc

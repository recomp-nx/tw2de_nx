/**
 * @file        fpu_est.h
 * @brief       fres / frsqrte: the hardware's estimates on AArch64
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Like the VMX estimates (vmx_fp.h), the FPU's reciprocal-square-root estimate is a table lookup, not an exact result:
 * frsqrte(1.0) is 0.970703125 on the hardware. tw2de (2026-10-24): both come from xenia-edge (BSD 3-Clause,
 * https://github.com/has207/xenia-edge, commit b3e8fda2):
 *  - frsqrte: its hardware-matched table (kEstimates in EmitFrsqrteHelper, src/xenia/cpu/backend/a64/a64_backend.cc),
 *    the same results as before;
 *  - fres: its single-precision reciprocal (InstrEmit_fresx, src/xenia/cpu/ppc/ppc_emit_fpu.cc): 1 / float(x), widened
 *    back to double. The hardware's fres is a 12-bit estimate (fres(1.0) is 0.99609375 there), so this is closer to
 *    the exact value than the hardware is: not the same bits (sdk/tests/fpu_estimates_test.cpp counts them).
 *
 * Elsewhere (x86) the previous expressions are kept: double(float(1.0 / x)) and double(float(1.0 / sqrt(x))).
 */

#pragma once

#include <rex/ppc/estimates.h>  // tw2de (2026-11-01)

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rex::ppc {

namespace fpu_est {
// frsqrte: the result's top 8 mantissa bits (double-precision bits 51..44; the rest 0) by (input exponent is even, input
// mantissa bits 51..49). Source: xenia-edge, kEstimates in EmitFrsqrteHelper (src/xenia/cpu/backend/a64/a64_backend.cc).
inline constexpr uint8_t kRsqrt[16] = {241, 216, 192, 168, 152, 136, 128, 112, 96, 76, 60, 48, 32, 24, 16, 8};

inline uint64_t bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
inline double from_bits(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
}  // namespace fpu_est

// fres: single-precision estimate of 1/x: xenia-edge's 1 / float(x) (see above).
inline double fres(double x) {
  // tw2de (2026-11-01): the 360's own estimate, bit for bit (rex/ppc/estimates.h: Xenia's table).
  return hw_est::EstimateReciprocalSingle(x);
}

// frsqrte: estimate of 1/sqrt(x) (double precision, 20 significant mantissa bits at most).
inline double frsqrte(double x) {
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_VMX_FP_PORTABLE)
  using namespace fpu_est;
  const uint64_t b = bits(x);
  const uint64_t sign = b & 0x8000000000000000ull;
  int64_t exp = int64_t((b >> 52) & 0x7FF);
  uint64_t man = b & 0xFFFFFFFFFFFFFull;
  if (exp == 0x7FF) {
    if (man) return from_bits(b | 0x0008000000000000ull);  // NaN: quieted
    return from_bits(sign ? 0x7FF8000000000000ull : 0);   // 1/sqrt(+INF) = +0, -INF: NaN
  }
  if (exp == 0 && man == 0) return from_bits(sign | 0x7FF0000000000000ull);  // +-0: +-INF
  if (sign) return from_bits(0x7FF8000000000000ull);                         // negative: NaN
  if (exp == 0) {  // denormal: normalized first (the hardware does, unlike its single-precision cousins)
    const int shift = __builtin_clzll(man) - 11;
    man = (man << shift) & 0xFFFFFFFFFFFFFull;
    exp = 1 - shift;
  }
  // Biased exponent of the result: 0x5FE - floor((e + 1) / 2) (= 1022 - floor((e - 1023 + 1) / 2) for 1/sqrt).
  const int64_t rexp = 0x5FE - ((exp + 1) >> 1);
  const uint32_t idx = uint32_t(((exp & 1) ^ 1) << 3) | uint32_t((man >> 49) & 7);
  return from_bits(uint64_t(rexp) << 52 | uint64_t(kRsqrt[idx]) << 44);
#else
  return double(float(1.0 / sqrt(x)));
#endif
}

}  // namespace rex::ppc

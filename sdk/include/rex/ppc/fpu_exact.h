/**
 * @file        fpu_exact.h
 * @brief       The FPU's results as the Xbox 360 gives them where the host's differ (codegen option exact_fp)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * tw2de (2026-11-01). Off by default (a game's code with valid inputs never sees a difference; each helper adds a
 * test of the result, or of the inputs for the single-precision ones). With exact_fp the generator writes these for
 * the arithmetic instructions. Every rule below was checked against Xenia's hardware-recorded instruction tests
 * (src/xenia/cpu/ppc/testing/instr__gen_*.s), case by case:
 *
 *  - A NaN operand: the result is the first NaN of frA, frB, frC (that order, frB being the addend of the multiply-adds),
 *    made quiet; the negated multiply-adds do not negate it, and fmsub does not negate frB's. The host picks a
 *    signaling NaN first and negates the addend before the operation (1456 cases each in fmadd, fmsub, fnmadd).
 *  - An invalid operation without a NaN operand: QNaN 0x7FF8000000000000 (AArch64's default NaN; x86's is negative).
 *  - The single-precision instructions with an operand that is not a single (a double denormal): undefined in the
 *    ISA, and on the 360: with a NaN operand the NaN as above (rounded to single), with an infinite one the result
 *    as usual, and otherwise QNaN 0x7FF8000000000000, except fdivs and fsqrts, which then give the double-precision
 *    result, not rounded to single (fadds, fsubs, fmuls, fmadds, fmsubs, fnmadds, fnmsubs, fdivs, fsqrts: all their
 *    cases).
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/platform/fpscr.h>
#include <rex/ppc/fpu_est.h>

namespace rex::ppc::exact {

inline uint64_t bits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}
inline double from_bits(uint64_t u) {
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}
inline bool nan(double x) { return x != x; }
inline double quiet(double x) { return from_bits(bits(x) | 0x0008000000000000ull); }
inline double default_nan() { return from_bits(0x7FF8000000000000ull); }
// A double the single-precision instructions do not take: a denormal (no single has its exponent).
inline bool not_single(double x) { return (bits(x) & 0x7FF0000000000000ull) == 0 && (bits(x) << 1) != 0; }
inline bool inf(double x) { return (bits(x) & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
// A NaN result rounded to single: the payload's low 29 bits go.
inline double nan_single(double x) { return from_bits(bits(quiet(x)) & 0xFFFFFFFFE0000000ull); }

// A signaling NaN operand raises invalid (FPSCR VXSNAN, VX, FX) where no host operation runs on it.
inline bool snan(double x) { return nan(x) && !(bits(x) & 0x0008000000000000ull); }
inline void raise_invalid() {
  volatile double z = 0.0;
  volatile double r = z / z;
  (void)r;
}

inline void raise_divzero() {
  volatile double z = 0.0, o = 1.0;
  volatile double r = o / z;
  (void)r;
}
inline void raise_overflow() {
  volatile double m = 1e308;
  volatile double r = m * m;
  (void)r;
}

// The result r of an operation on a (and b, c): r, or the NaN the 360 gives.
inline double pick(double r, double a) {
  if (!nan(r)) [[likely]]
    return r;
  return nan(a) ? quiet(a) : default_nan();
}
inline double pick(double r, double a, double b) {
  if (!nan(r)) [[likely]]
    return r;
  return nan(a) ? quiet(a) : nan(b) ? quiet(b) : default_nan();
}
inline double pick(double r, double a, double b, double c) {
  if (!nan(r)) [[likely]]
    return r;
  return nan(a) ? quiet(a) : nan(b) ? quiet(b) : nan(c) ? quiet(c) : default_nan();
}

// Keep the fused result in its register (the compiler would fold a negation into FNMADD / FMSUB: fpu_fma.h).
inline double keep(double t) {
#if defined(__aarch64__) || defined(_M_ARM64)
  __asm__("" : "+w"(t));
#elif defined(__x86_64__) || defined(__i386__)
  __asm__("" : "+x"(t));
#endif
  return t;
}

// Double precision.
inline double fadd(double a, double b) { return pick(a + b, a, b); }
inline double fsub(double a, double b) { return pick(a - b, a, b); }
inline double fmul(double a, double c) { return pick(a * c, a, c); }
inline double fdiv(double a, double b) { return pick(a / b, a, b); }
inline double fsqrt(double b) { return pick(std::sqrt(b), b); }
inline double fmadd(double a, double c, double b) { return pick(keep(std::fma(a, c, b)), a, b, c); }
inline double fmsub(double a, double c, double b) { return pick(keep(std::fma(a, c, -b)), a, b, c); }
inline double fnmadd(double a, double c, double b) {
  const double t = keep(std::fma(a, c, b));
  return nan(t) ? pick(t, a, b, c) : -t;
}
inline double fnmsub(double a, double c, double b) {
  const double t = keep(std::fma(a, c, -b));
  return nan(t) ? pick(t, a, b, c) : -t;
}

// Single precision: the operands' checks, then the double result rounded to single.
inline double single(double r) { return nan(r) ? nan_single(r) : double(float(r)); }
// The operands' special cases; true when `out` is the result. b and c may be repeated a.
inline bool single_special(double a, double b, double c, double& out) {
  const bool any_nan = nan(a) || nan(b) || nan(c);
  const bool any_not_single = not_single(a) || not_single(b) || not_single(c);
  if (!any_nan && !any_not_single) [[likely]]
    return false;
  if (any_nan) {
    if (snan(a) || snan(b) || snan(c))
      raise_invalid();
    out = nan_single(nan(a) ? a : nan(b) ? b : c);
    return true;
  }
  if (inf(a) || inf(b) || inf(c))
    return false;  // an infinity: the usual result
  out = default_nan();
  return true;
}
inline double fadds(double a, double b) {
  double o;
  return single_special(a, b, b, o) ? o : single(pick(a + b, a, b));
}
inline double fsubs(double a, double b) {
  double o;
  return single_special(a, b, b, o) ? o : single(pick(a - b, a, b));
}
inline double fmuls(double a, double c) {
  double o;
  return single_special(a, c, c, o) ? o : single(pick(a * c, a, c));
}
// 0 * infinity is invalid (VXIMZ) even when the addend is a NaN (the result is then that NaN).
inline void zero_times_inf(double a, double c) {
  if ((a == 0 && inf(c)) || (inf(a) && c == 0)) [[unlikely]]
    raise_invalid();
}

inline double fmadds(double a, double c, double b) {
  double o;
  zero_times_inf(a, c);
  return single_special(a, b, c, o) ? o : single(pick(keep(std::fma(a, c, b)), a, b, c));
}
inline double fmsubs(double a, double c, double b) {
  double o;
  zero_times_inf(a, c);
  return single_special(a, b, c, o) ? o : single(pick(keep(std::fma(a, c, -b)), a, b, c));
}
inline double fnmadds(double a, double c, double b) {
  double o;
  zero_times_inf(a, c);
  if (single_special(a, b, c, o))
    return o;
  const double t = keep(std::fma(a, c, b));
  return nan(t) ? single(pick(t, a, b, c)) : -single(t);  // rounded, then negated
}
inline double fnmsubs(double a, double c, double b) {
  double o;
  zero_times_inf(a, c);
  if (single_special(a, b, c, o))
    return o;
  const double t = keep(std::fma(a, c, -b));
  return nan(t) ? single(pick(t, a, b, c)) : -single(t);  // rounded, then negated
}
// fdivs / fsqrts: a NaN as above; a non-single operand gives the double-precision result.
inline double fdivs(double a, double b) {
  if (nan(a) || nan(b)) [[unlikely]] {
    if (snan(a) || snan(b))
      raise_invalid();
    return nan_single(nan(a) ? a : b);
  }
  if (not_single(a) || not_single(b)) [[unlikely]]
    return pick(a / b, a, b);
  return single(pick(a / b, a, b));
}
inline double fsqrts(double b) {
  if (nan(b)) [[unlikely]] {
    if (snan(b))
      raise_invalid();
    return nan_single(b);
  }
  if (not_single(b)) [[unlikely]]
    return std::sqrt(b);
  return single(pick(std::sqrt(b), b));
}

// fres / frsqrte: the estimates (rex/ppc/fpu_est.h), with the 360's flags only: a zero operand raises zero divide
// (ZX), fres of an operand whose reciprocal overflows raises overflow (OX), frsqrte of a negative operand and either
// of a signaling NaN raise invalid; nothing else (no inexact: the hardware-recorded tests' CR1).
inline double fres(double x) {
  const uint32_t saved = platform::FPSCRPlatform::saveStatus();
  const double r = rex::ppc::fres(x);
  platform::FPSCRPlatform::restoreStatus(saved);
  if (x == 0)
    raise_divzero();
  else if (snan(x))
    raise_invalid();
  else if (inf(r) && !inf(x))
    raise_overflow();
  return r;
}
inline double frsqrte(double x) {
  const uint32_t saved = platform::FPSCRPlatform::saveStatus();
  const double r = rex::ppc::frsqrte(x);
  platform::FPSCRPlatform::restoreStatus(saved);
  if (x == 0)
    raise_divzero();
  else if (snan(x) || (x < 0))
    raise_invalid();
  return r;
}

// exact_fp_log (tw2de 2026-11-01, diagnostic codegen option): the host's result h stays; where the 360's could
// differ, the exact result is computed (the host's flags kept) and a difference reported, once per instruction
// (src/core/ppc_exact_log.cpp). The double-precision instructions differ only in NaN results; the single-precision
// ones also for a NaN or non-single operand.
[[gnu::cold, gnu::noinline]] void report(uint32_t insn, uint32_t function, const char* op, uint64_t host,
                                         uint64_t x360);

template <typename F>
[[gnu::cold, gnu::noinline]] inline void check_slow(uint32_t insn, uint32_t function, const char* op, double h,
                                                    F exact) {
  const uint32_t saved = platform::FPSCRPlatform::saveStatus();
  const double e = exact();
  platform::FPSCRPlatform::restoreStatus(saved);
  if (bits(e) != bits(h))
    report(insn, function, op, bits(h), bits(e));
}
template <typename F>
inline void check(uint32_t insn, uint32_t function, const char* op, double h, F exact) {
  if (nan(h)) [[unlikely]]
    check_slow(insn, function, op, h, exact);
}
template <typename F>
inline void check_single(uint32_t insn, uint32_t function, const char* op, double h, F exact, double a, double b,
                         double c) {
  if (nan(h) || nan(a) || nan(b) || nan(c) || not_single(a) || not_single(b) || not_single(c)) [[unlikely]]
    check_slow(insn, function, op, h, exact);
}

// fp_origin_log (tw2de 2026-11-01, diagnostic codegen option): a result that is not finite (NaN or infinity) from
// operands that all are: where a NaN or an infinity starts. Reported once per instruction (src/core/ppc_exact_log.cpp).
// lr, sp, base: the guest's LR and r1 at the instruction and the guest memory, for the callers in the report (the
// return addresses on the guest stack's back chain).
[[gnu::cold, gnu::noinline]] void report_origin(uint32_t insn, uint32_t function, const char* op, uint64_t result,
                                                uint64_t a, uint64_t b, uint64_t c, uint32_t lr, uint32_t sp,
                                                const uint8_t* base);
inline bool finite(double x) { return (bits(x) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull; }
inline void origin(uint32_t insn, uint32_t function, const char* op, double h, double a, double b, double c,
                   uint32_t lr, uint32_t sp, const uint8_t* base) {
  if (!finite(h)) [[unlikely]]
    if (finite(a) && finite(b) && finite(c))
      report_origin(insn, function, op, bits(h), bits(a), bits(b), bits(c), lr, sp, base);
}

}  // namespace rex::ppc::exact

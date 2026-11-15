/**
 * @file        fpu_fma.h
 * @brief       fnmadd / fnmsub: the negated fused multiply-adds, with the sign of zero and NaN the PowerPC gives
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * tw2de (2026-11-01): fnmadd is -(a * c + b) and fnmsub -(a * c - b), and a NaN result keeps its sign (it is not
 * negated). The generated code was -std::fma(a, c, b) and -std::fma(a, c, -b). GCC (devkitA64) folds that negation into
 * the AArch64 instructions FNMADD (-(a * c) - b) and FMSUB (b - a * c), which are not the same on zeros: with
 * a * c = +0 and b = +0, fnmsub gives -0 on the PowerPC and FMSUB gives +0 (and fnmadd with b = -0 the same way).
 * Xenia's PowerPC instruction tests (src/xenia/cpu/ppc/testing, results recorded on the hardware) showed it: 80
 * fnmadd and 80 fnmsub cases giving +0 where the hardware gives -0. Here the fused result is kept in its register
 * (an empty asm with the value as operand: the compiler cannot fold across it) and then negated, unless it is NaN.
 */

#pragma once

#include <cmath>

namespace rex::ppc {

namespace fpu_fma {
inline double keep(double t) {
#if defined(__aarch64__) || defined(_M_ARM64)
  __asm__("" : "+w"(t));
#elif defined(__x86_64__) || defined(__i386__)
  __asm__("" : "+x"(t));
#endif
  return t;
}
}  // namespace fpu_fma

// fnmadd: -(a * c + b), fused; a NaN result is not negated.
inline double fnmadd(double a, double c, double b) {
  const double t = fpu_fma::keep(std::fma(a, c, b));
  return t != t ? t : -t;
}

// fnmsub: -(a * c - b), fused; a NaN result is not negated.
inline double fnmsub(double a, double c, double b) {
  const double t = fpu_fma::keep(std::fma(a, c, -b));
  return t != t ? t : -t;
}

}  // namespace rex::ppc

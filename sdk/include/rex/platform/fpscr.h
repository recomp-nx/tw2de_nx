/**
 * @file        platform/fpscr.h
 * @brief       Platform-specific FPSCR constants and intrinsics
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cmath>

#include <rex/types.h>

#include <simde/x86/sse.h>

// SSE3 constants are missing from simde
#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#endif

namespace rex::platform {

// tw2de (2026-11-01): the host's floating-point exception flags, as FPSCRPlatform::getStatus gives them.
constexpr u32 kStatusInvalid = 1, kStatusDivZero = 2, kStatusOverflow = 4, kStatusUnderflow = 8, kStatusInexact = 16;

// simde does not handle denormal flags, so we need to implement per-arch.
#if defined(__x86_64__) || defined(_M_X64)

struct FPSCRPlatform {
  static constexpr size_t RoundShift = 13;
  static constexpr size_t RoundMaskVal = SIMDE_MM_ROUND_MASK;
  static constexpr size_t FlushMask = SIMDE_MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
  // Exception mask bits (1 = exception masked/disabled)
  static constexpr u32 ExceptionMask = (1 << 7) |   // IM - Invalid operation
                                       (1 << 8) |   // DM - Denormal operand
                                       (1 << 9) |   // ZM - Zero divide
                                       (1 << 10) |  // OM - Overflow
                                       (1 << 11) |  // UM - Underflow
                                       (1 << 12);   // PM - Precision (Inexact)
  static constexpr size_t GuestToHost[] = {SIMDE_MM_ROUND_NEAREST, SIMDE_MM_ROUND_TOWARD_ZERO,
                                           SIMDE_MM_ROUND_UP, SIMDE_MM_ROUND_DOWN};

  static inline u32 getcsr() noexcept { return simde_mm_getcsr(); }

  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr); }

  static inline void InitHostExceptions(u32& csr) noexcept {
    csr |= ExceptionMask;  // Set mask bits to disable exceptions
  }

  // tw2de (2026-11-01): RC to the PowerPC's RN (RC: 0 nearest, 1 down, 2 up, 3 toward zero).
  static constexpr u32 HostToGuestRound[] = {0, 3, 2, 1};
  // The exception flags raised since the last clear, as kStatus* bits (MXCSR: IE 0, ZE 2, OE 3, UE 4, PE 5).
  static inline u32 getStatus() noexcept {
    const u32 m = simde_mm_getcsr();
    return ((m & 0x01) ? kStatusInvalid : 0) | ((m & 0x04) ? kStatusDivZero : 0) |
           ((m & 0x08) ? kStatusOverflow : 0) | ((m & 0x10) ? kStatusUnderflow : 0) |
           ((m & 0x20) ? kStatusInexact : 0);
  }
  static inline void clearStatus() noexcept { simde_mm_setcsr(simde_mm_getcsr() & ~0x3Fu); }
  // The flags as they are, and put back (a helper whose host operations raise flags the 360's instruction does not).
  static inline u32 saveStatus() noexcept { return simde_mm_getcsr() & 0x3Fu; }
  static inline void restoreStatus(u32 s) noexcept { simde_mm_setcsr((simde_mm_getcsr() & ~0x3Fu) | (s & 0x3Fu)); }
};

#elif defined(__aarch64__) || defined(_M_ARM64)

struct FPSCRPlatform {
  // RMode
  static constexpr size_t RoundShift = 22;
  static constexpr size_t RoundMaskVal = 3 << RoundShift;
  // FZ and FZ16
  static constexpr size_t FlushMask = (1 << 19) | (1 << 24);
  // Nearest, Zero, -Infinity, -Infinity
  static constexpr size_t GuestToHost[] = {0 << RoundShift, 3 << RoundShift, 1 << RoundShift,
                                           2 << RoundShift};
  // Exception enable bits (0 = exception disabled, ARM defaults to 0)
  static constexpr u32 ExceptionMask = (1 << 8) |   // IOE - Invalid Operation
                                       (1 << 9) |   // DZE - Division by Zero
                                       (1 << 10) |  // OFE - Overflow
                                       (1 << 11) |  // UFE - Underflow
                                       (1 << 12) |  // IXE - Inexact
                                       (1 << 15);   // IDE - Input Denormal

  static inline u32 getcsr() noexcept {
    u64 csr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr));
    return csr;
  }

  static inline void setcsr(u32 csr) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(csr)); }

  static inline void InitHostExceptions(u32& csr) noexcept {
    csr &= ~ExceptionMask;  // Clear enable bits to disable exceptions
  }

  // tw2de (2026-11-01): RMode to the PowerPC's RN (RMode: 0 nearest, 1 toward +inf, 2 toward -inf, 3 toward
  // zero; RN: 0 nearest, 1 toward zero, 2 toward +inf, 3 toward -inf). The x86 table was used here before.
  static constexpr u32 HostToGuestRound[] = {0, 2, 3, 1};
  // The exception flags raised since the last clear, as kStatus* bits (FPSR: IOC 0, DZC 1, OFC 2, UFC 3, IXC 4: the
  // same order).
  static inline u32 getStatus() noexcept {
    u64 sr;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(sr));
    return u32(sr) & 0x1F;
  }
  static inline void clearStatus() noexcept {
    u64 sr;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(sr));
    sr &= ~u64(0x9F);  // IOC DZC OFC UFC IXC and IDC; QC (saturation) and the rest kept
    __asm__ __volatile__("msr fpsr, %0" : : "r"(sr));
  }
  // The flags as they are, and put back (a helper whose host operations raise flags the 360's instruction does not).
  static inline u32 saveStatus() noexcept {
    u64 sr;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(sr));
    return u32(sr) & 0x9F;
  }
  static inline void restoreStatus(u32 s) noexcept {
    u64 sr;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(sr));
    sr = (sr & ~u64(0x9F)) | (s & 0x9F);
    __asm__ __volatile__("msr fpsr, %0" : : "r"(sr));
  }
};

#else
#error "Missing implementation for FPSCR."
#endif

}  // namespace rex::platform

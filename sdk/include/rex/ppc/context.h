/**
 * @file        ppc/context.h
 * @brief       PPC thread context, guest function macros, and interrupt handling
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp PPCContext infrastructure
 */

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/platform/fpscr.h>
#include <rex/ppc/diet_ops.h>
#include <rex/ppc/func.h>
#include <rex/sys_counters.h>
#include <rex/types.h>

#include <simde/x86/avx.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

//=============================================================================
// Pack/Unpack Constants (NORMPACKED32 - 2:10:10:10 format)
//=============================================================================

constexpr float kPack2101010_Min10 = std::bit_cast<float>(0x403FFE01u);
constexpr float kPack2101010_Max10 = std::bit_cast<float>(0x404001FFu);
constexpr float kPack2101010_Min2 = std::bit_cast<float>(0x40400000u);
constexpr float kPack2101010_Max2 = std::bit_cast<float>(0x40400003u);

namespace rex::ppc {
// TW2 (2026-11-12, switch_flush_mode_fixed, sdk/src/system/xthread.cpp): the host's flush-to-zero mode is left as each
// guest thread starts (off: IEEE, the 360 FPU's) instead of being switched on for VMX code and off for FPU code. Every
// switch is an FPCR write (~44 ns on the console); mixed FPU / VMX code (animation blends, audio DSP) switched per
// element: 4.5% of all CPU samples in the prologue. The only difference: a VMX result or input below 2^-126 is kept
// (denormal) instead of flushed to zero. Set once, before any guest thread runs.
extern bool g_flush_mode_fixed;
}  // namespace rex::ppc

namespace rex::ppc {

//=============================================================================
// General Purpose Register
//=============================================================================

union Register {
  int8_t s8;
  uint8_t u8;
  int16_t s16;
  uint16_t u16;
  int32_t s32;
  uint32_t u32;
  int64_t s64;
  uint64_t u64;
  float f32;
  double f64;
};

//=============================================================================
// Fixed-Point Exception Register (XER)
//=============================================================================

struct XERRegister {
  uint8_t so;
  uint8_t ov;
  uint8_t ca;
};

//=============================================================================
// Condition Register (CR) Field
//=============================================================================

struct CRRegister {
  uint8_t lt;
  uint8_t gt;
  uint8_t eq;
  union {
    uint8_t so;
    uint8_t un;
  };

  inline uint32_t raw() const noexcept { return (lt << 3) | (gt << 2) | (eq << 1) | so; }

  inline void set_raw(uint32_t value) noexcept {
    lt = (value >> 3) & 1;
    gt = (value >> 2) & 1;
    eq = (value >> 1) & 1;
    so = value & 1;
  }

  template <typename T>
  inline void compare(T left, T right, const XERRegister& xer) noexcept {
    lt = left < right;
    gt = left > right;
    eq = left == right;
    so = xer.so;
  }

  inline void compare(double left, double right) noexcept {
#ifndef REX_NO_FAST_FCMP
    // Mass Effect diet: same truth table, but the quiet comparison builtins let one fcmp serve all four
    // flags (the relational operators and == compiled to fcmpe and fcmp pairs plus NaN tests).
    un = __builtin_isunordered(left, right);
    lt = __builtin_isless(left, right);
    gt = __builtin_isgreater(left, right);
    eq = left == right;
#else
    un = __builtin_isnan(left) || __builtin_isnan(right);
    lt = !un && (left < right);
    gt = !un && (left > right);
    eq = !un && (left == right);
#endif
  }

  inline void setFromMask(simde__m128 mask, int imm) noexcept {
#if defined(__aarch64__) && !defined(REX_SETFROMMASK_MOVEMASK)
    // tw2de (2026-10-24): the record form of a vector compare (imm 0xF, 584 sites): movemask_ps is 0xF when
    // every word's sign bit is set and 0 when none is. Each word's sign as a 0 / -1 halfword (SSHR, XTN), one 64-bit
    // value compared twice, instead of simde's movemask (AND, USHL by a table, ADDV). Exact for any mask
    // (sdk/tests/vec_neon_forms_test.cpp).
    if (__builtin_constant_p(imm) && imm == 0xF) {
      const int32x4_t words = vreinterpretq_s32_f32(simde__m128_to_neon_f32(mask));
      const uint64_t signs =
          vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(vreinterpretq_u32_s32(vshrq_n_s32(words, 31)))), 0);
      lt = signs == ~uint64_t(0);
      gt = 0;
      eq = signs == 0;
      so = 0;
      return;
    }
#endif
    int m = simde_mm_movemask_ps(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }

  inline void setFromMask(simde__m128i mask, int imm) noexcept {
    int m = simde_mm_movemask_epi8(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }
};

//=============================================================================
// Vector Register (128-bit)
//=============================================================================

union alignas(0x10) VRegister {
  int8_t s8[16];
  uint8_t u8[16];
  int16_t s16[8];
  uint16_t u16[8];
  int32_t s32[4];
  uint32_t u32[4];
  int64_t s64[2];
  uint64_t u64[2];
  float f32[4];
  double f64[2];
};

//=============================================================================
// Floating-Point Status and Control Register (FPSCR)
//=============================================================================

constexpr uint32_t kRoundNearest = 0x00;
constexpr uint32_t kRoundTowardZero = 0x01;
constexpr uint32_t kRoundUp = 0x02;
constexpr uint32_t kRoundDown = 0x03;
constexpr uint32_t kRoundMask = 0x03;

struct FPSCRRegister {
  uint32_t csr;
  // tw2de (2026-11-01): the rest of the guest FPSCR (all but RN, which is the host's rounding mode, and FEX / VX,
  // which are computed): the exception bits, enables and NI as mtfsf / mtfsfi / mtfsb* wrote them, plus the host's
  // exception flags folded in (fold()). FPRF, FR and FI are not tracked (0). vx_host: the host said "invalid" without
  // saying which kind (it keeps VX set without one of the VX* bits).
  uint32_t guest = 0;
  uint32_t vx_host = 0;
  // tw2de (2026-11-01): the guest's rounding mode (FPSCR RN) in host bits. The vector unit always rounds to nearest
  // (the 360's VMX has no rounding mode), the FPU as RN says: the host mode follows the flush mode, which already
  // switches between the two (on: vector code, nearest; off: FPU code, RN). Before, a RN set by the game (mtfsf)
  // also changed how the vector code rounded.
  uint32_t guest_round = 0;

  static constexpr uint32_t kFX = 0x80000000u, kFEX = 0x40000000u, kVX = 0x20000000u, kOX = 0x10000000u,
                            kUX = 0x08000000u, kZX = 0x04000000u, kXX = 0x02000000u;
  static constexpr uint32_t kVXBits = 0x01F80700u;  // VXSNAN VXISI VXIDI VXZDZ VXIMZ VXVC VXSOFT VXSQRT VXCVI
  static constexpr uint32_t kExceptionBits = kOX | kUX | kZX | kXX | kVXBits;

  static constexpr size_t HostToGuest[] = {kRoundNearest, kRoundDown, kRoundUp, kRoundTowardZero};

  using Platform = platform::FPSCRPlatform;
  static constexpr size_t RoundShift = Platform::RoundShift;
  static constexpr size_t RoundMaskVal = Platform::RoundMaskVal;
  static constexpr size_t FlushMask = Platform::FlushMask;

  // Bits the guest owns; the rest is host policy seeded by InitHost.
  static constexpr uint32_t GuestMask = uint32_t(RoundMaskVal) | uint32_t(FlushMask);

  inline uint32_t getcsr() noexcept { return Platform::getcsr(); }
  inline void setcsr(uint32_t csr) noexcept { Platform::setcsr(csr); }

  // Restoring the whole word would unmask every FP exception when csr is 0.
  inline void restoreGuestBits(uint32_t saved) noexcept {
    csr = (getcsr() & ~GuestMask) | (saved & GuestMask);
    setcsr(csr);
  }

  inline uint32_t loadFromHost() noexcept {
    csr = getcsr();
    return Platform::HostToGuestRound[(guest_round & RoundMaskVal) >> RoundShift];
  }

  // The host's exception flags since the last fold into the guest bits (FX when one of them is new), then cleared.
  inline void fold() noexcept {
    const uint32_t s = Platform::getStatus();
    if (!s)
      return;
    uint32_t add = 0;
    if (s & platform::kStatusDivZero) add |= kZX;
    if (s & platform::kStatusOverflow) add |= kOX;
    if (s & platform::kStatusUnderflow) add |= kUX;
    if (s & platform::kStatusInexact) add |= kXX;
    const bool new_vx = (s & platform::kStatusInvalid) && !vx_host && !(guest & kVXBits);
    if (s & platform::kStatusInvalid) vx_host = 1;
    if ((add & ~guest) || new_vx)
      add |= kFX;
    guest |= add;
    Platform::clearStatus();
  }

  // mffs: the whole FPSCR.
  inline uint32_t read() noexcept {
    fold();
    uint32_t v = guest & ~(kFEX | kVX);
    if ((guest & kVXBits) || vx_host) v |= kVX;
    // FEX: an exception whose enable is set (VE 0x80, OE 0x40, UE 0x20, ZE 0x10, XE 0x08).
    const uint32_t en = guest;
    if (((v & kVX) && (en & 0x80)) || ((v & kOX) && (en & 0x40)) || ((v & kUX) && (en & 0x20)) ||
        ((v & kZX) && (en & 0x10)) || ((v & kXX) && (en & 0x08)))
      v |= kFEX;
    return v | loadFromHost();
  }

  // mtfsf / mtfsfi / mtfsb0 / mtfsb1: the bits of mask set from value (FEX and VX are not written: they are computed).
  inline void write(uint32_t value, uint32_t mask) noexcept {
    fold();
    // NI (0x4) reads 0 on the 360 whatever was written (Xenia's instruction tests: mtfsf of 5 reads back 1).
    mask &= ~(kFEX | kVX | 0x4u);
    guest = (guest & ~mask) | (value & mask & ~kRoundMask);
    if (mask & kVXBits) vx_host = 0;  // the VX* bits written: the unnamed one goes
    if (mask & kRoundMask)
      storeFromGuest((loadFromHost() & ~(mask & kRoundMask)) | (value & mask & kRoundMask));
  }

  // mtfsb1: one bit set; an exception bit that was clear sets FX too (mtfsf and mtfsfi do not).
  inline void setBit(uint32_t bit) noexcept {
    fold();
    const bool fresh = (bit & kExceptionBits) && !(guest & bit);
    write(bit, bit);
    if (fresh)
      guest |= kFX;
  }

  // The record forms' CR1: FX FEX VX OX.
  inline uint32_t cr1() noexcept { return read() >> 28; }

  // mcrfs: field n (0 = bits 0..3) to a CR field, its exception bits cleared (FEX and VX stay computed).
  inline uint32_t takeField(uint32_t n) noexcept {
    const uint32_t field = (read() >> (28 - 4 * n)) & 0xF;
    const uint32_t clear = (0xF0000000u >> (4 * n)) & (kFX | kExceptionBits);
    guest &= ~clear;
    if (clear & kVXBits) vx_host = 0;
    return field;
  }

  inline void storeFromGuest(uint32_t value) noexcept {
    REX_SYS_CENSUS((value & kRoundMask) == kRoundNearest  ? rex::syscount::kCensusRoundNearest
                   : (value & kRoundMask) == kRoundTowardZero ? rex::syscount::kCensusRoundZero
                                                              : rex::syscount::kCensusRoundUpDown,
                   1);
    guest_round = uint32_t(Platform::GuestToHost[value & kRoundMask]);
    if ((csr & FlushMask) == 0) {  // FPU mode: the guest's; vector mode keeps nearest until it ends
      csr = (csr & ~RoundMaskVal) | guest_round;
      setcsr(csr);
    }
  }

  inline void enableFlushModeUnconditional() noexcept {
    if (g_flush_mode_fixed) [[likely]]
      return;  // TW2 (2026-11-12): switch_flush_mode_fixed
    REX_SYS_CENSUS_SITE(rex::syscount::kCensusFlushWrite);
    if ((csr & FlushMask) == FlushMask) REX_SYS_CENSUS(rex::syscount::kCensusFlushSame, 1);
    csr = (csr | FlushMask) & ~RoundMaskVal;
    setcsr(csr);
  }

  inline void disableFlushModeUnconditional() noexcept {
    if (g_flush_mode_fixed) [[likely]]
      return;  // TW2 (2026-11-12): switch_flush_mode_fixed
    REX_SYS_CENSUS_SITE(rex::syscount::kCensusFlushWrite);
    if ((csr & FlushMask) == 0) REX_SYS_CENSUS(rex::syscount::kCensusFlushSame, 1);
    csr = (csr & ~(FlushMask | RoundMaskVal)) | guest_round;
    setcsr(csr);
  }

  inline void enableFlushMode() noexcept {
    if ((csr & FlushMask) != FlushMask && !g_flush_mode_fixed) [[unlikely]] {
      REX_SYS_CENSUS_SITE(rex::syscount::kCensusFlushWrite);
      csr = (csr | FlushMask) & ~RoundMaskVal;
      setcsr(csr);
    }
  }

  inline void disableFlushMode() noexcept {
    if ((csr & FlushMask) != 0 && !g_flush_mode_fixed) [[unlikely]] {
      REX_SYS_CENSUS_SITE(rex::syscount::kCensusFlushWrite);
      csr = (csr & ~(FlushMask | RoundMaskVal)) | guest_round;
      setcsr(csr);
    }
  }

  // tools/flush_mode_flow.py: the test of a check the control flow proved redundant (PC builds keep it).
  inline bool flushModeIsOn() const noexcept { return (csr & FlushMask) == FlushMask; }
  inline bool flushModeIsOff() const noexcept { return (csr & FlushMask) == 0; }

  inline void InitHost() noexcept {
    csr = getcsr();
    guest_round = csr & RoundMaskVal;
    Platform::InitHostExceptions(csr);
    setcsr(csr);
  }
};

}  // namespace rex::ppc

using PPCRegister = rex::ppc::Register;
using PPCXERRegister = rex::ppc::XERRegister;
using PPCCRRegister = rex::ppc::CRRegister;
using PPCVRegister = rex::ppc::VRegister;
using PPCFPSCRRegister = rex::ppc::FPSCRRegister;

//=============================================================================
// PPCContext Structure
//=============================================================================

struct alignas(0x40) PPCContext {
  PPCRegister r3;
  PPCRegister r0;
  PPCRegister r1;
  PPCRegister r2;
  PPCRegister r4;
  PPCRegister r5;
  PPCRegister r6;
  PPCRegister r7;
  PPCRegister r8;
  PPCRegister r9;
  PPCRegister r10;
  PPCRegister r11;
  PPCRegister r12;
  PPCRegister r13;
  PPCRegister r14;
  PPCRegister r15;
  PPCRegister r16;
  PPCRegister r17;
  PPCRegister r18;
  PPCRegister r19;
  PPCRegister r20;
  PPCRegister r21;
  PPCRegister r22;
  PPCRegister r23;
  PPCRegister r24;
  PPCRegister r25;
  PPCRegister r26;
  PPCRegister r27;
  PPCRegister r28;
  PPCRegister r29;
  PPCRegister r30;
  PPCRegister r31;

  uint64_t lr;
  PPCRegister ctr;
  PPCXERRegister xer;
  PPCRegister reserved;
  uint32_t msr = 0x200A000;
  PPCCRRegister cr0;
  PPCCRRegister cr1;
  PPCCRRegister cr2;
  PPCCRRegister cr3;
  PPCCRRegister cr4;
  PPCCRRegister cr5;
  PPCCRRegister cr6;
  PPCCRRegister cr7;
  PPCFPSCRRegister fpscr;
  uint8_t vscr_sat = 0;  // VSCR saturation flag (for vector ops)
  uint8_t vscr_nj = 1;   // tw2de (2026-11-01): VSCR NJ as mtvscr wrote it (1 at reset); the vector unit always flushes
  // tw2de (2026-11-01): lwarx / ldarx set it, stwcx. / stdcx. clear it: without a reservation the store fails.
  uint8_t reserve_valid = 0;

  /**
   * Last indirect call target address. Set by REX_CALL_INDIRECT_FUNC before
   * dispatch. Used by the invalid-function trap to report the faulting address.
   * Unconditional (not guarded by config flags) because ctr may be optimized
   * to a local variable via REX_CONFIG_CTR_AS_LOCAL.
   */
  uint32_t last_indirect_target = 0;

  PPCRegister f0;
  PPCRegister f1;
  PPCRegister f2;
  PPCRegister f3;
  PPCRegister f4;
  PPCRegister f5;
  PPCRegister f6;
  PPCRegister f7;
  PPCRegister f8;
  PPCRegister f9;
  PPCRegister f10;
  PPCRegister f11;
  PPCRegister f12;
  PPCRegister f13;
  PPCRegister f14;
  PPCRegister f15;
  PPCRegister f16;
  PPCRegister f17;
  PPCRegister f18;
  PPCRegister f19;
  PPCRegister f20;
  PPCRegister f21;
  PPCRegister f22;
  PPCRegister f23;
  PPCRegister f24;
  PPCRegister f25;
  PPCRegister f26;
  PPCRegister f27;
  PPCRegister f28;
  PPCRegister f29;
  PPCRegister f30;
  PPCRegister f31;

  PPCVRegister v0;
  PPCVRegister v1;
  PPCVRegister v2;
  PPCVRegister v3;
  PPCVRegister v4;
  PPCVRegister v5;
  PPCVRegister v6;
  PPCVRegister v7;
  PPCVRegister v8;
  PPCVRegister v9;
  PPCVRegister v10;
  PPCVRegister v11;
  PPCVRegister v12;
  PPCVRegister v13;
  PPCVRegister v14;
  PPCVRegister v15;
  PPCVRegister v16;
  PPCVRegister v17;
  PPCVRegister v18;
  PPCVRegister v19;
  PPCVRegister v20;
  PPCVRegister v21;
  PPCVRegister v22;
  PPCVRegister v23;
  PPCVRegister v24;
  PPCVRegister v25;
  PPCVRegister v26;
  PPCVRegister v27;
  PPCVRegister v28;
  PPCVRegister v29;
  PPCVRegister v30;
  PPCVRegister v31;
  PPCVRegister v32;
  PPCVRegister v33;
  PPCVRegister v34;
  PPCVRegister v35;
  PPCVRegister v36;
  PPCVRegister v37;
  PPCVRegister v38;
  PPCVRegister v39;
  PPCVRegister v40;
  PPCVRegister v41;
  PPCVRegister v42;
  PPCVRegister v43;
  PPCVRegister v44;
  PPCVRegister v45;
  PPCVRegister v46;
  PPCVRegister v47;
  PPCVRegister v48;
  PPCVRegister v49;
  PPCVRegister v50;
  PPCVRegister v51;
  PPCVRegister v52;
  PPCVRegister v53;
  PPCVRegister v54;
  PPCVRegister v55;
  PPCVRegister v56;
  PPCVRegister v57;
  PPCVRegister v58;
  PPCVRegister v59;
  PPCVRegister v60;
  PPCVRegister v61;
  PPCVRegister v62;
  PPCVRegister v63;
  PPCVRegister v64;
  PPCVRegister v65;
  PPCVRegister v66;
  PPCVRegister v67;
  PPCVRegister v68;
  PPCVRegister v69;
  PPCVRegister v70;
  PPCVRegister v71;
  PPCVRegister v72;
  PPCVRegister v73;
  PPCVRegister v74;
  PPCVRegister v75;
  PPCVRegister v76;
  PPCVRegister v77;
  PPCVRegister v78;
  PPCVRegister v79;
  PPCVRegister v80;
  PPCVRegister v81;
  PPCVRegister v82;
  PPCVRegister v83;
  PPCVRegister v84;
  PPCVRegister v85;
  PPCVRegister v86;
  PPCVRegister v87;
  PPCVRegister v88;
  PPCVRegister v89;
  PPCVRegister v90;
  PPCVRegister v91;
  PPCVRegister v92;
  PPCVRegister v93;
  PPCVRegister v94;
  PPCVRegister v95;
  PPCVRegister v96;
  PPCVRegister v97;
  PPCVRegister v98;
  PPCVRegister v99;
  PPCVRegister v100;
  PPCVRegister v101;
  PPCVRegister v102;
  PPCVRegister v103;
  PPCVRegister v104;
  PPCVRegister v105;
  PPCVRegister v106;
  PPCVRegister v107;
  PPCVRegister v108;
  PPCVRegister v109;
  PPCVRegister v110;
  PPCVRegister v111;
  PPCVRegister v112;
  PPCVRegister v113;
  PPCVRegister v114;
  PPCVRegister v115;
  PPCVRegister v116;
  PPCVRegister v117;
  PPCVRegister v118;
  PPCVRegister v119;
  PPCVRegister v120;
  PPCVRegister v121;
  PPCVRegister v122;
  PPCVRegister v123;
  PPCVRegister v124;
  PPCVRegister v125;
  PPCVRegister v126;
  PPCVRegister v127;

  //--- Host scratch (rexglue ports; sasrt-nx 2026-10-12) --------
  // Per-thread counters for the host's hooks, reached through the context every hook is handed: no thread_local
  // (on the Switch each access is a call to __aarch64_read_tp) and no counter shared by all threads (a cache line
  // bouncing between cores on every hooked call). Not guest state: never saved, restored or compared; a context
  // copied and copied back only repeats a few counts. Zero in a new thread's context (value-initialised).
  static constexpr size_t kHostCounters = 64;
  uint32_t host_counters[kHostCounters];

  //--- Non-volatile register save/restore --------
  // Layout: r14-r31 (144) | f14-f31 (144) | v14-v31 (288) | v64-v127 (1024)
  //       | cr2-cr4 (12) | fpscr (4).  Total: 1616 bytes.
  static constexpr size_t kNonVolatileSaveSize =
      18 * sizeof(PPCRegister) + 18 * sizeof(PPCRegister) + 18 * sizeof(PPCVRegister) +
      64 * sizeof(PPCVRegister) + 3 * sizeof(PPCCRRegister) + sizeof(PPCFPSCRRegister);

  inline void SaveNonVolatiles(uint8_t* dst) const {
    std::memcpy(dst, &r14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &f14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &v14, 18 * sizeof(PPCVRegister));
    dst += 18 * sizeof(PPCVRegister);
    std::memcpy(dst, &v64, 64 * sizeof(PPCVRegister));
    dst += 64 * sizeof(PPCVRegister);
    std::memcpy(dst, &cr2, 3 * sizeof(PPCCRRegister));
    dst += 3 * sizeof(PPCCRRegister);
    std::memcpy(dst, &fpscr, sizeof(PPCFPSCRRegister));
  }

  inline void RestoreNonVolatiles(const uint8_t* src) {
    std::memcpy(&r14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&f14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&v14, src, 18 * sizeof(PPCVRegister));
    src += 18 * sizeof(PPCVRegister);
    std::memcpy(&v64, src, 64 * sizeof(PPCVRegister));
    src += 64 * sizeof(PPCVRegister);
    std::memcpy(&cr2, src, 3 * sizeof(PPCCRRegister));
    src += 3 * sizeof(PPCCRRegister);
    PPCFPSCRRegister saved_fpscr;
    std::memcpy(&saved_fpscr, src, sizeof(PPCFPSCRRegister));
    fpscr.restoreGuestBits(saved_fpscr.csr);
  }
};

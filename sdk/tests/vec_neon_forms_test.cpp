// tools/vec_neon_forms.py: each new form against the generated code's old one, on AArch64 (qemu), bit for bit.
//
//   python3 tools/vec_neon_forms.py --dump-macros > /tmp/vec_neon_forms_macros.h
//   aarch64-linux-gnu-g++ -std=gnu++23 -O2 -static -march=armv8-a+crc+crypto -ffp-contract=off -frounding-math
//       -Isdk/include -Isdk/thirdparty/simde -Isdk/thirdparty/simde/simde -I/tmp
//       sdk/tests/vec_neon_forms_test.cpp -o /tmp/vnf && qemu-aarch64 /tmp/vnf
//
// vslw: every count 0..31 (and counts with the bits above 4 set, which the generator masks), random words.
// SCVTF #n: n = 1, 15 (signed) and 31 (unsigned) as the game has them, and every n 1..32, over every integer near a
// rounding boundary (24 significant bits and up) and random ones, with the four rounding modes and flush to zero off
// and on. vupkd3d types 0 and 4: random registers and every value of each byte / halfword they read.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include <rex/ppc/intrinsics.h>
#include "vec_neon_forms_macros.h"  // the pch's (tools/vec_neon_forms.py --dump-macros)

namespace {

uint64_t g_checks = 0, g_diff = 0;

bool Same(simde__m128i a, simde__m128i b) {
  ++g_checks;
  uint8_t x[16], y[16];
  simde_mm_storeu_si128((simde__m128i *)x, a);
  simde_mm_storeu_si128((simde__m128i *)y, b);
  if (std::memcmp(x, y, 16) == 0)
    return true;
  ++g_diff;
  return false;
}
bool Same(simde__m128 a, simde__m128 b) { return Same(simde_mm_castps_si128(a), simde_mm_castps_si128(b)); }

// Through a register the compiler cannot see into: constant inputs were folded at compile time, in the default
// rounding mode, while the other form ran in the mode the test set.
simde__m128i Words(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  int32x4_t v = simde__m128i_to_neon_i32(simde_mm_setr_epi32(a, b, c, d));
  asm volatile("" : "+w"(v));
  return simde__m128i_from_neon_i32(v);
}

void SetFpcr(uint64_t v) { asm volatile("msr fpcr, %0" ::"r"(v) : "memory"); }

// The old forms, as the generator writes them.
simde__m128i OldSllv(simde__m128i a, simde__m128i b) {
  simde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi32(0x1F));
  return simde_mm_sllv_epi32(a, shift);
}
simde__m128i NewSllv(simde__m128i a, simde__m128i b) {
  simde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi32(0x1F));
  return REX_SLLV31(a, shift);
}

simde__m128 OldCvt(simde__m128i x, int n) {
  return simde_mm_mul_ps(simde_mm_cvtepi32_ps(x), simde_mm_castsi128_ps(simde_mm_set1_epi32(int((127 - n) << 23))));
}
simde__m128 OldCvtU(simde__m128i x, int n) {
  return simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(x),
                         simde_mm_castsi128_ps(simde_mm_set1_epi32(int((127 - n) << 23))));
}
template <int n> simde__m128 NewCvt(simde__m128i x) { return REX_CVT_SCALED(x, n); }
template <int n> simde__m128 NewCvtU(simde__m128i x) { return REX_CVTU_SCALED(x, n); }

union VReg {
  uint8_t u8[16];
  int16_t s16[8];
  uint32_t u32[4];
  int32_t s32[4];
  float f32[4];
};
union Temp {
  uint32_t u32;
  int32_t s32;
  float f32;
};

simde__m128i OldColor(simde__m128i v) {
  VReg src, vTemp;
  simde_mm_storeu_si128((simde__m128i *)src.u8, v);
  vTemp.u32[0] = src.u8[3] | 0x3F800000;
  vTemp.u32[1] = src.u8[0] | 0x3F800000;
  vTemp.u32[2] = src.u8[1] | 0x3F800000;
  vTemp.u32[3] = src.u8[2] | 0x3F800000;
  return simde_mm_loadu_si128((simde__m128i *)vTemp.u8);
}
simde__m128i OldShort2(simde__m128i v, int32_t &temp_out) {
  VReg src, vTemp;
  Temp temp;
  simde_mm_storeu_si128((simde__m128i *)src.u8, v);
  temp.f32 = 3.0f;
  temp.s32 += src.s16[1];
  vTemp.f32[3] = temp.f32;
  temp.f32 = 3.0f;
  temp.s32 += src.s16[0];
  vTemp.f32[2] = temp.f32;
  vTemp.f32[1] = 0.0f;
  vTemp.f32[0] = 1.0f;
  temp_out = temp.s32;
  return simde_mm_loadu_si128((simde__m128i *)vTemp.u8);
}
simde__m128i NewShort2(simde__m128i v, int32_t &temp_out) {
  VReg src;
  simde_mm_storeu_si128((simde__m128i *)src.u8, v);
  temp_out = 0x40400000 + src.s16[0];
  return REX_VUPKD3D_SHORT2(v);
}

template <int n> void CvtSweep(std::mt19937 &rng, bool with_unsigned) {
  auto one = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const simde__m128i x = Words(a, b, c, d);
    if (!Same(OldCvt(x, n), NewCvt<n>(x)) && g_diff < 8)
      std::printf("cvt n=%d differs at %08X\n", n, a);
    if (with_unsigned && !Same(OldCvtU(x, n), NewCvtU<n>(x)) && g_diff < 8)
      std::printf("cvtu n=%d differs at %08X\n", n, a);
  };
  // Around every power of two from 2^23 up (where rounding happens), both signs.
  for (int e = 23; e < 32; ++e) {
    const uint32_t base = 1u << e;
    for (uint32_t k = 0; k < 4096; k += 4) {
      one(base + k, base - k, base + k + 1, base + (k << (e - 23)));
      one(uint32_t(-int64_t(base + k)), uint32_t(-int64_t(base - k)), 0x80000000u + k, 0xFFFFFFFFu - k);
    }
  }
  for (int i = 0; i < 200000; ++i)
    one(rng(), rng(), rng(), rng());
  one(0, 1, 0xFFFFFFFFu, 0x80000000u);
  one(0x7FFFFFFFu, 0x7FFFFFC0u, 0x7FFFFFBFu, 0x7FFFFF40u);
}

template <int imm> void PermOne(std::mt19937 &rng) {
  for (int i = 0; i < 2000; ++i) {
    const simde__m128i v = Words(rng(), rng(), rng(), rng());
    if (!Same(simde_mm_shuffle_epi32(v, imm), REX_PERMWI(v, imm)) && g_diff < 8)
      std::printf("vpermwi %02X differs\n", imm);
  }
}
template <int... is> void PermAll(std::mt19937 &rng, std::integer_sequence<int, is...>) { (PermOne<is>(rng), ...); }

template <int... ns> void CvtAll(std::mt19937 &rng, std::integer_sequence<int, ns...>) {
  (CvtSweep<ns + 1>(rng, true), ...);
}

}  // namespace

int main() {
  std::mt19937 rng(20261024);
  // vslw
  for (uint32_t c = 0; c < 64; ++c)
    for (int i = 0; i < 20000; ++i) {
      const simde__m128i a = Words(rng(), rng(), rng(), rng());
      const simde__m128i b = Words(c, c ^ 0x20, rng(), c | 0xFFFFFF00u);
      if (!Same(OldSllv(a, b), NewSllv(a, b)) && g_diff < 8)
        std::printf("vslw differs, count %u\n", c);
    }
  std::printf("vslw: %llu checks, %llu differences\n", (unsigned long long)g_checks, (unsigned long long)g_diff);
  // SCVTF #n under each rounding mode, flush to zero off and on (FPCR.RMode 23:22, FZ 24).
  const uint64_t base_checks = g_checks, base_diff = g_diff;
  for (uint64_t mode = 0; mode < 4; ++mode)
    for (uint64_t fz = 0; fz < 2; ++fz) {
      SetFpcr((mode << 22) | (fz << 24));
      CvtAll(rng, std::make_integer_sequence<int, 32>());
    }
  SetFpcr(0);
  std::printf("vcsxwfp / vcuxwfp: %llu checks, %llu differences\n", (unsigned long long)(g_checks - base_checks),
              (unsigned long long)(g_diff - base_diff));
  // vupkd3d
  const uint64_t up_checks = g_checks, up_diff = g_diff;
  for (int i = 0; i < 2000000; ++i) {
    const simde__m128i v = Words(rng(), rng(), rng(), rng());
    Same(OldColor(v), REX_VUPKD3D_COLOR(v));
    int32_t t0, t1;
    const simde__m128i a = OldShort2(v, t0), b = NewShort2(v, t1);
    Same(a, b);
    ++g_checks;
    g_diff += t0 != t1;
  }
  for (uint32_t h = 0; h < 65536; ++h)
    for (int lane = 0; lane < 2; ++lane) {
      const uint32_t w = lane ? (h << 16) | (rng() & 0xFFFF) : (rng() << 16) | h;
      const simde__m128i v = Words(w, rng(), rng(), rng());
      int32_t t0, t1;
      Same(OldShort2(v, t0), NewShort2(v, t1));
      ++g_checks;
      g_diff += t0 != t1;
      const simde__m128i c = Words(h * 0x01010101u ^ rng(), rng(), rng(), rng());
      Same(OldColor(c), REX_VUPKD3D_COLOR(c));
    }
  std::printf("vupkd3d 0 / 4: %llu checks, %llu differences\n", (unsigned long long)(g_checks - up_checks),
              (unsigned long long)(g_diff - up_diff));
  // vpermwi: every immediate.
  const uint64_t pw_checks = g_checks, pw_diff = g_diff;
  PermAll(rng, std::make_integer_sequence<int, 256>());
  std::printf("vpermwi: %llu checks, %llu differences\n", (unsigned long long)(g_checks - pw_checks),
              (unsigned long long)(g_diff - pw_diff));
  // CR6 of a record-form compare (rex/ppc/context.h setFromMask, imm 0xF): the movemask form against the sign
  // halfwords, over every combination of sign bits with random low bits, and random masks.
  const uint64_t cr_checks = g_checks, cr_diff = g_diff;
  for (int i = 0; i < 4000000; ++i) {
    uint32_t w[4];
    for (int k = 0; k < 4; ++k)
      w[k] = (i & 1) ? rng() : ((((i >> 1) >> k) & 1) ? 0x80000000u : 0u) | (rng() & (i & 2 ? 0x7FFFFFFFu : 0u));
    const simde__m128 mask = simde_mm_castsi128_ps(Words(w[0], w[1], w[2], w[3]));
    const int m = simde_mm_movemask_ps(mask);
    const bool lt0 = m == 0xF, eq0 = m == 0;
    const int32x4_t words = vreinterpretq_s32_f32(simde__m128_to_neon_f32(mask));
    const uint64_t signs =
        vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(vreinterpretq_u32_s32(vshrq_n_s32(words, 31)))), 0);
    ++g_checks;
    g_diff += (lt0 != (signs == ~uint64_t(0))) || (eq0 != (signs == 0));
  }
  std::printf("CR6 from a mask: %llu checks, %llu differences\n", (unsigned long long)(g_checks - cr_checks),
              (unsigned long long)(g_diff - cr_diff));
  std::printf("all: %llu checks, %llu differences\n", (unsigned long long)g_checks, (unsigned long long)g_diff);
  return g_diff ? 1 : 0;
}

#!/usr/bin/env python3
"""tw2de (2026-10-24): guest vector forms GCC compiles poorly, written as the NEON they are (exact, bit for bit).

Each was compiled with the Switch build's flags and read (the notes' vecsurvey: the generated statement as it is, and
the new one), then checked against the old form under qemu-aarch64 (sdk/tests/vec_neon_forms_test.cpp):
  - vslw (485 sites): the generator masks the counts to 0..31, then simde's _mm_sllv_epi32 checks again for counts of
    32 or more (CMHS and AND around the USHL). REX_SLLV31: the USHL alone. 9 instructions -> 3.
  - vupkd3d128 type 0, D3DCOLOR (658 sites): four byte loads, four ORRs, two stores to a temporary and a copy.
    REX_VUPKD3D_COLOR: one TBL (bytes 3, 0, 1, 2 to the low byte of each word, the rest 0) and one ORR with 1.0f.
  - vupkd3d128 type 4, NORMSHORT2 (229 sites): two halfword loads, two integer adds on 3.0f's bits, lane stores.
    REX_VUPKD3D_SHORT2: SXTL, ADD, and the constant half {1.0f, 0}.
  - vpermwi128 (82 sites): simde's _mm_shuffle_epi32 is lane by lane on NEON (4 instructions for every immediate but
    the four splats, the identity included). REX_PERMWI: the same lanes as a compile-time shuffle, which GCC makes one
    EXT / REV64 / ZIP / UZP / DUP (or nothing) or a TBL: used only for the 198 immediates where that is shorter (the
    splats of vspltw stay simde's, one DUP already).
  - vcsxwfp128 / vcuxwfp128 with a scale (736 sites): SCVTF then FMUL by 2^-n. REX_CVT_SCALED: SCVTF #n, which rounds
    x / 2^n once; the old form rounded x, then scaled it exactly (a power of two, and |x| / 2^n >= 2^-31 is never
    subnormal): the same float for every x.
The temporaries the old forms left behind (vTemp, temp) are set to the same values (temp by the old form's own last
two statements; the compiler drops them when nothing reads them), so the rest of the function sees what it saw.

Every rewrite is idempotent; --restore puts the old forms back. REX_VEC_NEON_FORMS_OFF (or not AArch64) compiles the
macros as the old expressions.

usage: vec_neon_forms.py --gen <app/generated/default> [--restore] | --dump-macros
"""
import glob
import os
import re
import sys

PCH_MACRO = r'''
// tw2de (2026-10-24): tools/vec_neon_forms.py. Guest vector forms as the NEON they are (exact).
#ifndef REX_VEC_NEON_FORMS
#define REX_VEC_NEON_FORMS
#if defined(__aarch64__) && !defined(REX_VEC_NEON_FORMS_OFF)
// vslw with counts already masked to 0..31: USHL.
#define REX_SLLV31(a, s) \
  simde__m128i_from_neon_u32(vshlq_u32(simde__m128i_to_neon_u32(a), simde__m128i_to_neon_i32(s)))
// x / 2^n converted once (SCVTF / UCVTF #n).
#define REX_CVT_SCALED(x, n) simde__m128_from_neon_f32(vcvtq_n_f32_s32(simde__m128i_to_neon_i32(x), n))
#define REX_CVTU_SCALED(x, n) simde__m128_from_neon_f32(vcvtq_n_f32_u32(simde__m128i_to_neon_u32(x), n))
// vupkd3d128 type 0: word k = byte {3, 0, 1, 2}[k] | 0x3F800000.
static inline simde__m128i rex_vupkd3d_color(simde__m128i v) {
  static const uint8_t idx[16] = {3, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0xFF, 1, 0xFF, 0xFF, 0xFF, 2, 0xFF, 0xFF, 0xFF};
  return simde__m128i_from_neon_u32(vorrq_u32(
      vreinterpretq_u32_u8(vqtbl1q_u8(simde__m128i_to_neon_u8(v), vld1q_u8(idx))), vdupq_n_u32(0x3F800000u)));
}
// vupkd3d128 type 4: {1.0f, 0.0f, 3.0f's bits + s16[0], 3.0f's bits + s16[1]}.
static inline simde__m128i rex_vupkd3d_short2(simde__m128i v) {
  const int32x4_t wide = vmovl_s16(vget_low_s16(simde__m128i_to_neon_i16(v)));
  const int32x2_t hi = vadd_s32(vget_low_s32(wide), vdup_n_s32(0x40400000));
  return simde__m128i_from_neon_i32(vcombine_s32(vcreate_s32(0x3F800000ull), hi));
}
#else
#define REX_SLLV31(a, s) simde_mm_sllv_epi32((a), (s))
#define REX_CVT_SCALED(x, n) \
  simde_mm_mul_ps(simde_mm_cvtepi32_ps(x), simde_mm_castsi128_ps(simde_mm_set1_epi32(int((127 - (n)) << 23))))
#define REX_CVTU_SCALED(x, n) \
  simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(x), simde_mm_castsi128_ps(simde_mm_set1_epi32(int((127 - (n)) << 23))))
static inline simde__m128i rex_vupkd3d_color(simde__m128i v) {
  alignas(16) uint8_t b[16];
  alignas(16) uint32_t w[4];
  simde_mm_store_si128((simde__m128i *)b, v);
  w[0] = b[3] | 0x3F800000u;
  w[1] = b[0] | 0x3F800000u;
  w[2] = b[1] | 0x3F800000u;
  w[3] = b[2] | 0x3F800000u;
  return simde_mm_load_si128((const simde__m128i *)w);
}
static inline simde__m128i rex_vupkd3d_short2(simde__m128i v) {
  alignas(16) int16_t h[8];
  alignas(16) int32_t w[4];
  simde_mm_store_si128((simde__m128i *)h, v);
  w[0] = 0x3F800000;
  w[1] = 0;
  w[2] = 0x40400000 + h[0];
  w[3] = 0x40400000 + h[1];
  return simde_mm_load_si128((const simde__m128i *)w);
}
#endif
#if defined(__aarch64__) && !defined(REX_VEC_NEON_FORMS_OFF)
// vpermwi128 / _mm_shuffle_epi32 with an immediate: lane i is lane (imm >> 2i) & 3.
template <int imm> static inline simde__m128i rex_permwi(simde__m128i v) {
  const uint32x4_t u = simde__m128i_to_neon_u32(v);
  return simde__m128i_from_neon_u32(
      __builtin_shufflevector(u, u, imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3));
}
#define REX_PERMWI(v, imm) rex_permwi<(imm)>(v)
#else
#define REX_PERMWI(v, imm) simde_mm_shuffle_epi32((v), (imm))
#endif
#define REX_VUPKD3D_COLOR(v) rex_vupkd3d_color(v)
#define REX_VUPKD3D_SHORT2(v) rex_vupkd3d_short2(v)
#endif
'''

V = r'ctx\.v\d+'

# The immediates whose compile-time shuffle is shorter than simde's (the notes' vecsurvey: all 256 compiled both ways).
PERMWI_BETTER = {0x01,0x02,0x03,0x04,0x06,0x07,0x08,0x09,0x0B,0x0C,0x0D,0x0E,0x11,0x12,0x13,0x14,0x16,0x17,0x18,0x19,0x1B,0x1C,0x1D,0x1E,0x21,0x22,0x23,0x24,0x26,0x27,0x28,0x29,0x2B,0x2C,0x2D,0x2E,0x31,0x32,0x33,0x34,0x36,0x37,0x38,0x39,0x3B,0x3C,0x3D,0x3E,0x41,0x42,0x43,0x44,0x46,0x47,0x48,0x49,0x4B,0x4C,0x4D,0x4E,0x50,0x51,0x52,0x53,0x54,0x56,0x57,0x58,0x59,0x5B,0x5C,0x5D,0x5E,0x61,0x62,0x63,0x64,0x66,0x67,0x68,0x69,0x6B,0x6C,0x6D,0x6E,0x71,0x72,0x73,0x74,0x76,0x77,0x78,0x79,0x7B,0x7C,0x7D,0x7E,0x81,0x82,0x83,0x84,0x86,0x87,0x88,0x89,0x8B,0x8C,0x8D,0x8E,0x91,0x92,0x93,0x94,0x96,0x97,0x98,0x99,0x9B,0x9C,0x9D,0x9E,0xA0,0xA1,0xA2,0xA3,0xA4,0xA6,0xA7,0xA8,0xA9,0xAB,0xAC,0xAD,0xAE,0xB1,0xB2,0xB3,0xB4,0xB6,0xB7,0xB8,0xB9,0xBB,0xBC,0xBD,0xBE,0xC1,0xC2,0xC3,0xC4,0xC6,0xC7,0xC8,0xC9,0xCB,0xCC,0xCD,0xCE,0xD1,0xD2,0xD3,0xD4,0xD6,0xD7,0xD8,0xD9,0xDB,0xDC,0xDD,0xDE,0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEB,0xEC,0xED,0xEE,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE}
SHUF = re.compile(r'simde_mm_shuffle_epi32\((simde_mm_load_si128\(\(simde__m128i\*\)ctx\.v\d+\.u32\)), (0x[0-9A-F]{2})\)')
SHUF_BACK = re.compile(r'REX_PERMWI\((simde_mm_load_si128\(\(simde__m128i\*\)ctx\.v\d+\.u32\)), (0x[0-9A-F]{2})\)')

# vslw: the block's last statement, after the masked count.
SLLV_PREV = re.compile(r'^\t\tsimde__m128i shift = simde_mm_and_si128\(b, simde_mm_set1_epi32\(0x1F\)\);$')
SLLV = ('simde_mm_sllv_epi32(a, shift)', 'REX_SLLV31(a, shift)')

# vcsxwfp / vcuxwfp with a power-of-two scale: 2^-n is the float (127 - n) << 23.
CVT = re.compile(r'simde_mm_mul_ps\(simde_mm_cvtepi32_ps\((simde_mm_load_si128\(\(simde__m128i\*\)ctx\.v\d+\.u32\))\), '
                 r'simde_mm_castsi128_ps\(simde_mm_set1_epi32\(int\(0x([0-9A-F]{8})\)\)\)\)')
CVTU = re.compile(r'simde_mm_mul_ps\(rex::ppc::simde_mm_cvtepu32_ps_\((simde_mm_load_si128\(\(simde__m128i\*\)ctx\.v\d+\.u32\))\), '
                  r'simde_mm_castsi128_ps\(simde_mm_set1_epi32\(int\(0x([0-9A-F]{8})\)\)\)\)')
CVT_BACK = re.compile(r'REX_CVT_SCALED\((simde_mm_load_si128\(\(simde__m128i\*\)ctx\.v\d+\.u32\)), (\d+)\)')
CVTU_BACK = re.compile(r'REX_CVTU_SCALED\((simde_mm_load_si128\(\(simde__m128i\*\)ctx\.v\d+\.u32\)), (\d+)\)')


def scale_bits(n):
    return '%08X' % ((127 - n) << 23)


def scale_n(bits):
    """n for a constant that is 2^-n with 1 <= n <= 32 (SCVTF #n's range), else None."""
    b = int(bits, 16)
    if b & 0x807FFFFF:
        return None
    n = 127 - (b >> 23)
    return n if 1 <= n <= 32 else None


# vupkd3d128 types 0 and 4: the generator's lines, exactly.
def color_lines(d, s):
    return ['\tvTemp.u32[0] = %s.u8[3] | 0x3F800000;' % s, '\tvTemp.u32[1] = %s.u8[0] | 0x3F800000;' % s,
            '\tvTemp.u32[2] = %s.u8[1] | 0x3F800000;' % s, '\tvTemp.u32[3] = %s.u8[2] | 0x3F800000;' % s,
            '\t%s = vTemp;' % d]


def short2_lines(d, s):
    return ['\ttemp.f32 = 3.0f;', '\ttemp.s32 += %s.s16[1];' % s, '\tvTemp.f32[3] = temp.f32;', '\ttemp.f32 = 3.0f;',
            '\ttemp.s32 += %s.s16[0];' % s, '\tvTemp.f32[2] = temp.f32;', '\tvTemp.f32[1] = 0.0f;',
            '\tvTemp.f32[0] = 1.0f;', '\t%s = vTemp;' % d]


def color_new(d, s):
    return ['\tsimde_mm_store_si128((simde__m128i*)vTemp.u8, REX_VUPKD3D_COLOR(simde_mm_load_si128((simde__m128i*)%s.u8)));'
            % s, '\t%s = vTemp;' % d]


def short2_new(d, s):
    return ['\tsimde_mm_store_si128((simde__m128i*)vTemp.u8, REX_VUPKD3D_SHORT2(simde_mm_load_si128((simde__m128i*)%s.u8)));'
            % s, '\ttemp.f32 = 3.0f;', '\ttemp.s32 += %s.s16[0];' % s, '\t%s = vTemp;' % d]


UPK = re.compile(r'^\t// vupkd3d128 v(\d+),v(\d+),([04])$')


def rewrite(lines, counts):
    out = []
    i = 0
    while i < len(lines):
        l = lines[i]
        m = UPK.match(l)
        if m:
            d, s, t = 'ctx.v' + m.group(1), 'ctx.v' + m.group(2), m.group(3)
            old = color_lines(d, s) if t == '0' else short2_lines(d, s)
            if lines[i + 1:i + 1 + len(old)] == old:
                out.append(l)
                out.extend(color_new(d, s) if t == '0' else short2_new(d, s))
                counts['vupkd3d' + t] += 1
                i += 1 + len(old)
                continue
        if SLLV[0] in l and out and SLLV_PREV.match(out[-1]):
            l = l.replace(SLLV[0], SLLV[1])
            counts['vslw'] += 1
        if 'simde_mm_shuffle_epi32(' in l:
            def shuf(m):
                if int(m.group(2), 16) not in PERMWI_BETTER:
                    return m.group(0)
                counts['vpermwi'] += 1
                return 'REX_PERMWI(%s, %s)' % (m.group(1), m.group(2))
            l = SHUF.sub(shuf, l)
        if 'simde_mm_mul_ps(' in l:
            def cvt(m, macro, key):
                n = scale_n(m.group(2))
                if n is None:
                    return m.group(0)
                counts[key] += 1
                return '%s(%s, %d)' % (macro, m.group(1), n)
            l = CVT.sub(lambda m: cvt(m, 'REX_CVT_SCALED', 'vcsxwfp'), l)
            l = CVTU.sub(lambda m: cvt(m, 'REX_CVTU_SCALED', 'vcuxwfp'), l)
        out.append(l)
        i += 1
    return out


COLOR_NEW = re.compile(r'^\tsimde_mm_store_si128\(\(simde__m128i\*\)vTemp\.u8, REX_VUPKD3D_COLOR\(simde_mm_load_si128\('
                       r'\(simde__m128i\*\)(ctx\.v\d+)\.u8\)\)\);$')
SHORT2_NEW = re.compile(r'^\tsimde_mm_store_si128\(\(simde__m128i\*\)vTemp\.u8, REX_VUPKD3D_SHORT2\(simde_mm_load_si128\('
                        r'\(simde__m128i\*\)(ctx\.v\d+)\.u8\)\)\);$')


def restore(lines):
    out = []
    i = 0
    while i < len(lines):
        l = lines[i]
        m = COLOR_NEW.match(l)
        if m and i + 1 < len(lines):
            d = lines[i + 1].strip()[:-len(' = vTemp;')]
            out.extend(color_lines(d, m.group(1)))
            i += 2
            continue
        m = SHORT2_NEW.match(l)
        if m and i + 3 < len(lines):
            d = lines[i + 3].strip()[:-len(' = vTemp;')]
            out.extend(short2_lines(d, m.group(1)))
            i += 4
            continue
        l = l.replace(SLLV[1], SLLV[0])
        l = SHUF_BACK.sub(lambda m: 'simde_mm_shuffle_epi32(%s, %s)' % (m.group(1), m.group(2)), l)
        l = CVT_BACK.sub(lambda m: 'simde_mm_mul_ps(simde_mm_cvtepi32_ps(%s), simde_mm_castsi128_ps(simde_mm_set1_epi32('
                         'int(0x%s))))' % (m.group(1), scale_bits(int(m.group(2)))), l)
        l = CVTU_BACK.sub(lambda m: 'simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(%s), simde_mm_castsi128_ps('
                          'simde_mm_set1_epi32(int(0x%s))))' % (m.group(1), scale_bits(int(m.group(2)))), l)
        out.append(l)
        i += 1
    return out


def main():
    args = sys.argv[1:]
    if '--dump-macros' in args:  # for sdk/tests/vec_neon_forms_test.cpp
        sys.stdout.write(PCH_MACRO)
        return
    gen = args[args.index('--gen') + 1]
    files = sorted(glob.glob(os.path.join(gen, '*_recomp[._]*.cpp')))
    if '--restore' in args:
        for path in files:
            t = open(path, encoding='utf-8').read()
            if 'REX_SLLV31' in t or 'REX_CVT' in t or 'REX_VUPKD3D' in t or 'REX_PERMWI' in t:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(restore(t.split('\n'))))
        return
    pch = glob.glob(os.path.join(gen, '*_pch.h'))
    if pch:
        p = open(pch[0], encoding='utf-8').read()
        if 'REX_VEC_NEON_FORMS' not in p:
            open(pch[0], 'w', encoding='utf-8', newline='\n').write(p + PCH_MACRO)
    counts = {'vslw': 0, 'vupkd3d0': 0, 'vupkd3d4': 0, 'vcsxwfp': 0, 'vcuxwfp': 0, 'vpermwi': 0}
    for path in files:
        t = open(path, encoding='utf-8').read()
        if not ('simde_mm_sllv_epi32(a, shift)' in t or 'vupkd3d128' in t or 'simde_mm_cvtepi32_ps' in t or
                'cvtepu32_ps_' in t or 'simde_mm_shuffle_epi32(' in t):
            continue
        before = dict(counts)
        new = rewrite(t.split('\n'), counts)
        if counts != before:
            open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(new))
    print('vec_neon_forms: ' + ', '.join('%s %d' % kv for kv in counts.items()))


if __name__ == '__main__':
    main()

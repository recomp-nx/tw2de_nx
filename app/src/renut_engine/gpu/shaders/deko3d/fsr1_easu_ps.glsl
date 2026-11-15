#version 460
// tw2de-dk ubo=0x6 ssbo=0x0 tex=1
// tw2de (2026-10-25, tw2de_renut_upscale_filter 3, the boot menu's "Upscaler: FSR1"): the first pass of AMD FidelityFX
// Super Resolution 1.0, EASU (edge adaptive spatial upsampling), drawn instead of the game's 1024x576 -> 1280x720 upscale
// pixel shader (render/draw.cpp kGameUpscalePs); the game's vertex shader and draw stay. FSR1's second pass, RCAS
// (sharpening), runs on the upscale's target right after this draw, before the game draws its UI (fsr1_rcas_ps.glsl,
// render/fsr1.cpp).
//
// Credits: AMD FidelityFX Super Resolution 1.0, ffx_fsr1.h (Copyright (c) 2021 Advanced Micro Devices, Inc., MIT licence;
// github.com/GPUOpen-Effects/FidelityFX-FSR). This is its 32-bit EASU path (FsrEasuF with FsrEasuSetF and FsrEasuTapF,
// FSR_EASU_F) ported to GLSL, with its approximate reciprocals (APrxLoRcpF1, APrxLoRsqF1) and the same arithmetic.
// Adapted for TW2DE: fixed to 1024x576 -> 1280x720 (FsrEasuCon's constants as constants); the input position from
// the draw's texture coordinate (texcoord * size - 0.5 is FsrEasuCon's pixel * con0.xy + con0.zw), so the draw's
// viewport does not matter; the texture of fetch constant 0 from the shared constants with Renut's linear clamp sampler
// (gathers ignore the filter); alpha bilinear from the source, as the other upscalers filter it (FSR1 is colour only).
#extension GL_ARB_bindless_texture : require
#define EASU_IN_SIZE vec2(textureSize(src, 0))

// SharedConstants (render/draw.cpp): word 0 is the 2D texture of fetch constant 0 (its descriptor in bits 0-19).
layout(binding = 2, std140) uniform RenutUboShared { uvec4 v[85]; } g_UboShared;

layout(location = 0) in vec4 iTexCoord0;

// tw2de (2026-11-07, dynamic resolution; render/dynamic_resolution.h): the drawn fraction of a texture resolved from a
// dynamic-resolution target (its descriptor index bits 20-25: 0 none, n 1 - (n - 1) x SharedConstants' drs_step at
// byte 1356); 1 for the rest.
float DrsFill(uint word)
{
    const uint code = (word >> 20u) & 63u;
    return code == 0u ? 1.0 : 1.0 - float(code - 1u) * uintBitsToFloat(g_UboShared.v[84].w);
}
layout(location = 0) out vec4 oC0;

// The draw's pixel shader constants: c0.xy the source picture's size in texels (the game's upscale draw sets it).
// tw2de (2026-11-07): FsrEasuCon from the texture's size, not c0 (stale with a 1280x720 scene; any source size: 1600x900 to 1920x1080 docked too; the output's size is
// not needed, the input position comes from the texture coordinate).
layout(binding = 1, std140) uniform RenutUboPixel { vec4 v[257]; } g_UboPixel;

float APrxLoRcpF1(float a) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(a)); }
float APrxLoRsqF1(float a) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(a) >> 1u)); }
vec3 AMin3F3(vec3 x, vec3 y, vec3 z) { return min(x, min(y, z)); }
vec3 AMax3F3(vec3 x, vec3 y, vec3 z) { return max(x, max(y, z)); }

// A tap at integer offset (kx, ky) from texel f (FsrEasuTapF): its offset from the position, rotated by the direction
// and scaled by the anisotropic length, is (kx dx + ky dy - P, ky dx - kx dy - Q) * len with P and Q the position's
// part, worked out once; the squared lengths are folded into lx2 and ly2. The same arithmetic as AMD's, regrouped.
#define EASU_TAP(kx, ky, c) { \
    const float vx = (float(kx) * dir.x + float(ky) * dir.y) - P; \
    const float vy = (float(ky) * dir.x - float(kx) * dir.y) - Q; \
    const float d2 = min(vx * vx * lx2 + vy * vy * ly2, clp); \
    /* AMD's (25/16) (2/5 d2 - 1)^2 - (25/16 - 1) expanded: d2 (d2 / 4 - 5/4) + 1 (constants exact in short immediates) */ \
    const float wB = d2 * (0.25 * d2 - 1.25) + 1.0; \
    float wA = lob * d2 - 1.0; \
    wA *= wA; \
    const float w = wB * wA; \
    aC += (c) * w; \
    aW += w; }

// Accumulate direction and length.
void FsrEasuSetF(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV,
                 float lA, float lB, float lC, float lD, float lE)
{
    // Compute bilinear weight, branches factor out as predicates are compiler time immediates.
    //  s t
    //  u v
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;
    // Direction is the '+' diff.
    //    a
    //  b c d
    //    e
    // Then takes magnitude from abs average of both sides of 'c'.
    // Length converts gradient reversal to 0, smoothly to non-reversal at 1, shaped, then adding horz and vert terms.
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = APrxLoRcpF1(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    // Repeat for the y axis.
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = APrxLoRcpF1(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void main()
{
    sampler2D src = sampler2D(uvec2(g_UboShared.v[0].x & 0xFFFFFu, 0u));
    // The same texture with Renut's point clamp sampler (slot 1) for the colour loads at texel centres.
    sampler2D src_point = sampler2D(uvec2((g_UboShared.v[0].x & 0xFFFFFu) | (1u << 20u), 0u));

    // tw2de (2026-11-07): the part drawn (dynamic resolution) is the input; the texture's size for its coordinates.
    const float fill = DrsFill(g_UboShared.v[0].x);
    const vec2 kInSize = EASU_IN_SIZE * fill;
    const vec2 inv = 1.0 / EASU_IN_SIZE;
    const vec4 kCon1 = vec4(inv.x, inv.y, inv.x, -inv.y);
    const vec4 kCon2 = vec4(-inv.x, 2.0 * inv.y, inv.x, 2.0 * inv.y);
    const vec4 kCon3 = vec4(0.0, 4.0 * inv.y, 0.0, 0.0);
    vec2 pp = iTexCoord0.xy * kInSize - vec2(0.5);
    vec2 fp = floor(pp);
    pp -= fp;
    vec2 p0 = fp * kCon1.xy + kCon1.zw;
    vec2 p1 = p0 + kCon2.xy;
    vec2 p2 = p0 + kCon2.zw;
    vec2 p3 = p0 + kCon3.xy;
    // The lumas first (FSR's luma times 2: 0.5 r + g + 0.5 b), from the 12 texels' gathers; their colours are gathered
    // again where the taps use them (the same 12 texels, in the texture cache), so the 36 colour values are not all
    // live across the direction analysis.
    vec4 bczzL = textureGather(src, p0, 2) * 0.5 + (textureGather(src, p0, 0) * 0.5 + textureGather(src, p0, 1));
    vec4 ijfeL = textureGather(src, p1, 2) * 0.5 + (textureGather(src, p1, 0) * 0.5 + textureGather(src, p1, 1));
    vec4 klhgL = textureGather(src, p2, 2) * 0.5 + (textureGather(src, p2, 0) * 0.5 + textureGather(src, p2, 1));
    vec4 zzonL = textureGather(src, p3, 2) * 0.5 + (textureGather(src, p3, 0) * 0.5 + textureGather(src, p3, 1));
    vec2 dir = vec2(0.0);
    float len = 0.0;
    FsrEasuSetF(dir, len, pp, true, false, false, false, bczzL.x, ijfeL.w, ijfeL.z, klhgL.w, ijfeL.y);
    FsrEasuSetF(dir, len, pp, false, true, false, false, bczzL.y, ijfeL.z, klhgL.w, klhgL.z, klhgL.x);
    FsrEasuSetF(dir, len, pp, false, false, true, false, ijfeL.z, ijfeL.x, ijfeL.y, klhgL.x, zzonL.w);
    FsrEasuSetF(dir, len, pp, false, false, false, true, klhgL.w, ijfeL.y, klhgL.x, klhgL.y, zzonL.z);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = APrxLoRsqF1(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = APrxLoRcpF1(lob);
    const float P = pp.x * dir.x + pp.y * dir.y;
    const float Q = pp.y * dir.x - pp.x * dir.y;
    const float lx2 = len2.x * len2.x, ly2 = len2.y * len2.y;
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    // The colours by texel: the texel centres with the point clamp sampler (the texel itself, the image's edge
    // clamped as the gathers are), offsets from f as immediates. A load of another kind than the gathers
    // above, so the compiler keeps them apart: the 36 colour values are not all live across the direction analysis.
    const vec2 fc = (fp + 0.5) * kCon1.xy;
#define EASU_TEXEL(x, y) textureLodOffset(src_point, fc, 0.0, ivec2(x, y)).rgb
    EASU_TAP(0.0, -1.0, EASU_TEXEL(0, -1))  // b
    EASU_TAP(1.0, -1.0, EASU_TEXEL(1, -1))  // c
    EASU_TAP(-1.0, 1.0, EASU_TEXEL(-1, 1))  // i
    EASU_TAP(-1.0, 0.0, EASU_TEXEL(-1, 0))  // e
    // The next loads below a test on the weights so far that always passes (a weight sum is never -1e30): the
    // compiler does not hoist them above it, so the colours of 4 texels are live at a time, not 12.
    if (aW != -1e30) {
    EASU_TAP(2.0, 1.0, EASU_TEXEL(2, 1))   // l
    EASU_TAP(2.0, 0.0, EASU_TEXEL(2, 0))   // h
    EASU_TAP(1.0, 2.0, EASU_TEXEL(1, 2))   // o
    EASU_TAP(0.0, 2.0, EASU_TEXEL(0, 2))   // n
    }
    vec3 min4, max4;
    if (aW != -1e30) {
    // The 4 nearest last: they also give the dering limits.
    const vec3 f = EASU_TEXEL(0, 0), g = EASU_TEXEL(1, 0), j = EASU_TEXEL(0, 1), k = EASU_TEXEL(1, 1);
    EASU_TAP(0.0, 0.0, f)
    EASU_TAP(1.0, 0.0, g)
    EASU_TAP(0.0, 1.0, j)
    EASU_TAP(1.0, 1.0, k)
    min4 = min(min(f, g), min(j, k));
    max4 = max(max(f, g), max(j, k));
    }
#undef EASU_TEXEL
    oC0 = vec4(min(max4, max(min4, aC * vec3(1.0 / aW))), texture(src, iTexCoord0.xy * fill).a);
}

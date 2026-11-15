#version 460
// tw2de-dk ubo=0x20 ssbo=0x0 tex=1
// tw2de (2026-11-07): the scan-out (present_ps.glsl) with AMD FSR 1's EASU (fsr1_easu_ps.glsl's, at any ratio; no RCAS): a 1280x720
// scene's picture to the 1920x1080 output docked on a 1080p TV (the boot menu's Upscaler: FSR 1). Then the gamma of
// present_ps. EASU: AMD FidelityFX Super Resolution 1.0, ffx_fsr1.h (Copyright (c) 2021 Advanced
// Micro Devices, Inc., MIT licence), as ported in fsr1_easu_ps.glsl.
#extension GL_ARB_bindless_texture : require
#define dkh(t, s) uvec2(uint(t) | (uint(s) << 20u), 0u)

layout(std140, binding = 5) uniform type_ConstantBuffer_UtilityPush
{
    vec2 scale;
    vec2 translate;
    uint textureIndex;
    uint samplerIndex;
    float param0;
    float param1;
} g_Push;

layout(location = 0) in vec2 in_var_TEXCOORD0;
layout(location = 0) out vec4 out_var_SV_Target0;


// present_ps.glsl's gamma (param0 the power, param1 a ramp texture's slot or 0) and opaque alpha.
vec4 PresentGamma(vec4 c)
{
    vec3 g = pow(clamp(c.xyz, vec3(0.0), vec3(1.0)), vec3(g_Push.param0));
    uint ramp = floatBitsToUint(g_Push.param1);
    if (ramp != 0u) {
        ivec3 i = ivec3(roundEven(clamp(g, vec3(0.0), vec3(1.0)) * 255.0));
        sampler2D r = sampler2D(uvec2(ramp, 0u));
        g = vec3(texelFetch(r, ivec2(i.x, 0), 0).x, texelFetch(r, ivec2(i.y, 0), 0).y, texelFetch(r, ivec2(i.z, 0), 0).z);
    }
    return vec4(g, 1.0);
}

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
    sampler2D src = sampler2D(dkh(g_Push.textureIndex, g_Push.samplerIndex));
    // The same texture with Renut's point clamp sampler (slot 1) for the colour loads at texel centres.
    sampler2D src_point = sampler2D(dkh(g_Push.textureIndex, 1u));

    const vec2 kInSize = vec2(textureSize(src, 0));
    const vec2 inv = 1.0 / kInSize;
    const vec4 kCon1 = vec4(inv.x, inv.y, inv.x, -inv.y);
    const vec4 kCon2 = vec4(-inv.x, 2.0 * inv.y, inv.x, 2.0 * inv.y);
    const vec4 kCon3 = vec4(0.0, 4.0 * inv.y, 0.0, 0.0);
    vec2 pp = in_var_TEXCOORD0 * kInSize - vec2(0.5);
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
    out_var_SV_Target0 = PresentGamma(vec4(min(max4, max(min4, aC * vec3(1.0 / aW))), 1.0));
}

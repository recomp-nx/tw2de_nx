#version 460
// tw2de-dk ubo=0x20 ssbo=0x0 tex=1
// tw2de (2026-10-25, tw2de_renut_upscale_filter 3, the boot menu's "Upscaler: FSR1"): the second pass of AMD
// FidelityFX Super Resolution 1.0, RCAS (robust contrast adaptive sharpening), on the 1280x720 picture EASU drew
// (fsr1_easu_ps.glsl), before the game draws its UI over it (render/fsr1.cpp). Drawn with the utility vertex shader
// (fullscreen_vs: a full-screen triangle) into the upscale's target, red, green and blue (its alpha kept).
//
// Credits: AMD FidelityFX Super Resolution 1.0, ffx_fsr1.h (Copyright (c) 2021 Advanced Micro Devices, Inc., MIT licence;
// github.com/GPUOpen-Effects/FidelityFX-FSR). Its 32-bit RCAS path (FsrRcasF, FSR_RCAS_F) ported to GLSL with the same
// arithmetic, its approximate reciprocal (APrxMedRcpF1) and its noise removal (FSR_RCAS_DENOISE) off.
// Push block (utility.hlsli's UtilityPush): textureIndex = a copy of the picture (texel loads), param0 = FsrRcasCon's
// sharpness as exp2(-stops) (1.0 the sharpest), samplerIndex = the point clamp sampler, scale = 1 / the size.
#extension GL_ARB_bindless_texture : require

layout(std140, binding = 5) uniform type_ConstantBuffer_UtilityPush
{
    vec2 scale;
    vec2 translate;
    uint textureIndex;
    uint samplerIndex;
    float param0;
    float param1;
} g_Push;

layout(location = 0) out vec4 out_var_SV_Target0;

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

float APrxMedRcpF1(float a)
{
    float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(a));
    return b * (-b * a + 2.0);
}
float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }

void main()
{
    // The copy with Renut's point clamp sampler (samplerIndex): the 5 texels at the pixel's centre and its neighbours,
    // the edges clamped by the sampler (no clamps in the shader), offsets as immediates.
    sampler2D src = sampler2D(uvec2(g_Push.textureIndex | (g_Push.samplerIndex << 20u), 0u));
    const vec2 uv = gl_FragCoord.xy * g_Push.scale;
    // Algorithm uses minimal 3x3 pixel neighborhood.
    //    b
    //  d e f
    //    h
    vec3 b = textureLodOffset(src, uv, 0.0, ivec2(0, -1)).rgb;
    vec3 d = textureLodOffset(src, uv, 0.0, ivec2(-1, 0)).rgb;
    vec3 e = textureLod(src, uv, 0.0).rgb;
    vec3 f = textureLodOffset(src, uv, 0.0, ivec2(1, 0)).rgb;
    vec3 h = textureLodOffset(src, uv, 0.0, ivec2(0, 1)).rgb;
    // Min and max of ring.
    float mn4R = min(AMin3F1(b.r, d.r, f.r), h.r);
    float mn4G = min(AMin3F1(b.g, d.g, f.g), h.g);
    float mn4B = min(AMin3F1(b.b, d.b, f.b), h.b);
    float mx4R = max(AMax3F1(b.r, d.r, f.r), h.r);
    float mx4G = max(AMax3F1(b.g, d.g, f.g), h.g);
    float mx4B = max(AMax3F1(b.b, d.b, f.b), h.b);
    // Immediate constants for peak range.
    const vec2 peakC = vec2(1.0, -1.0 * 4.0);
    // Limiters, these need to be high precision RCPs.
    float hitMinR = min(mn4R, e.r) * (1.0 / (4.0 * mx4R));
    float hitMinG = min(mn4G, e.g) * (1.0 / (4.0 * mx4G));
    float hitMinB = min(mn4B, e.b) * (1.0 / (4.0 * mx4B));
    float hitMaxR = (peakC.x - max(mx4R, e.r)) * (1.0 / (4.0 * mn4R + peakC.y));
    float hitMaxG = (peakC.x - max(mx4G, e.g)) * (1.0 / (4.0 * mn4G + peakC.y));
    float hitMaxB = (peakC.x - max(mx4B, e.b)) * (1.0 / (4.0 * mn4B + peakC.y));
    float lobeR = max(-hitMinR, hitMaxR);
    float lobeG = max(-hitMinG, hitMaxG);
    float lobeB = max(-hitMinB, hitMaxB);
    float lobe = max(-FSR_RCAS_LIMIT, min(AMax3F1(lobeR, lobeG, lobeB), 0.0)) * g_Push.param0;
    // Resolve, which needs the medium precision rcp approximation to avoid visible tonality changes.
    float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
    vec3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    out_var_SV_Target0 = vec4(pix, 1.0);
}

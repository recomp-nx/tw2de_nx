#version 460
// tw2de-dk ubo=0x20 ssbo=0x0 tex=1
// tw2de (2026-11-07): the scan-out (present_ps.glsl) with Lanczos-2 (lanczos2_ps.glsl's, at any ratio): a 1280x720
// scene's picture to the 1920x1080 output docked on a 1080p TV (the boot menu's Upscaler: Lanczos). Then the gamma of
// present_ps. Written for TW2DE; not game code.
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

// Lanczos-2: sinc(x) sinc(x / 2).
float Lanczos2(float x)
{
    const float PI = 3.14159265;
    x = max(abs(x), 1e-4);
    return 2.0 * sin(PI * x) * sin(0.5 * PI * x) / (PI * PI * x * x);
}
vec4 LanczosAxis(float f)
{
    const float a = Lanczos2(f), b = Lanczos2(1.0 - f);
    return vec4(a + b, b / (a + b), Lanczos2(1.0 + f), Lanczos2(2.0 - f));
}

void main()
{
    // The texture with Renut's linear clamp sampler (slot 0): the middle fetch relies on bilinear filtering.
    sampler2D src = sampler2D(dkh(g_Push.textureIndex, g_Push.samplerIndex));
    // tw2de (2026-11-07): any source size (c0.xy, the game's own constant: 1600x900 to 1920x1080 docked too), and the
    // kernel's weights computed from each pixel's fraction (exact Lanczos-2) instead of the five rows of the fixed
    // 1024x576 -> 1280x720 ratio.
    const vec2 size = vec2(textureSize(src, 0));
    const vec2 inv = 1.0 / size;

    // Source position in texels, and the texel centre below-left of it.
    const vec2 src_pos = in_var_TEXCOORD0 * size;
    const vec2 src_centre = floor(src_pos - 0.5) + 0.5;
    const vec2 f = src_pos - src_centre;
    // Per axis: x = the inner pair's weight (L(f) + L(1 - f)), y = the middle fetch's offset from the centre,
    // z = the outer tap before the pair (L(1 + f)), w = the one after it (L(2 - f)).
    const vec4 px = LanczosAxis(f.x);
    const vec4 py = LanczosAxis(f.y);

    const vec2 w12 = vec2(px.x, py.x);
    vec4 wedge = vec4(px.z * w12.y, py.z * w12.x, px.w * w12.y, py.w * w12.x);

    const vec2 tc12 = inv * (src_centre + vec2(px.y, py.y));
    const vec2 tc0 = inv * (src_centre - 1.0);
    const vec2 tc3 = inv * (src_centre + 2.0);

    const float sum = wedge.x + wedge.y + wedge.z + wedge.w + w12.x * w12.y;
    wedge /= sum;
    // tw2de (2026-11-08): the middle fetch's weight divided by the sum too (it was not: the total weight went
    // 1.000-1.026 with the pixel's phase, a faint grid repeating every 6 pixels at 1600x900 -> 1920x1080).
    const float centre = w12.x * w12.y / sum;

    out_var_SV_Target0 = PresentGamma(texture(src, vec2(tc12.x, tc0.y)) * wedge.y +
          texture(src, vec2(tc0.x, tc12.y)) * wedge.x +
          texture(src, tc12) * centre +
          texture(src, vec2(tc3.x, tc12.y)) * wedge.z +
          texture(src, vec2(tc12.x, tc3.y)) * wedge.w);
}

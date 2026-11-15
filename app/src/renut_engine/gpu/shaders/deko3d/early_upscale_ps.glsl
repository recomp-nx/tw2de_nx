#version 460
// tw2de-dk ubo=0x20 ssbo=0x0 tex=1
// tw2de (2026-11-10): the scene's picture upscaled with Lanczos-2 before the post-processing when FSR 3.1 is off and
// the scene is smaller than the output (render/early_upscale.cpp: 1024x576 -> 1280x720), so the post-processing and
// the UI draw at the output's size. present_lanczos_ps.glsl's kernel without its gamma, alpha kept (filtered as
// colour). Drawn with fullscreen_vs. Written for TW2DE; not game code.
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
    // The texture can be larger than the picture (the scene's target): translate = the picture's size in texels,
    // fetches clamped to its last texel centre (scale, in texture coordinates).
    const vec2 inv = 1.0 / vec2(textureSize(src, 0));
    const vec2 src_pos = in_var_TEXCOORD0 * g_Push.translate;
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

    // Clamped to the picture's last texel centre (g_Push.scale), not the texture's edge.
    const vec2 lim = g_Push.scale;
    // param0 (tw2de_early_upscale_filter): 1 = bilinear, 2 = nearest texel; else Lanczos-2.
    if (g_Push.param0 > 0.5) {
        const vec2 p = g_Push.param0 > 1.5 ? (floor(src_pos) + 0.5) * inv : src_pos * inv;
        out_var_SV_Target0 = texture(src, min(p, lim));
        return;
    }
    out_var_SV_Target0 = max(vec4(0.0), texture(src, min(vec2(tc12.x, tc0.y), lim)) * wedge.y +
          texture(src, min(vec2(tc0.x, tc12.y), lim)) * wedge.x +
          texture(src, min(tc12, lim)) * centre +
          texture(src, min(vec2(tc3.x, tc12.y), lim)) * wedge.z +
          texture(src, min(vec2(tc12.x, tc3.y), lim)) * wedge.w);
}

#version 460
// tw2de-dk ubo=0x6 ssbo=0x0 tex=1
// tw2de (2026-10-25, tw2de_renut_upscale_filter 1): Renut's own upscaler, drawn instead of the game's 1024x576 ->
// 1280x720 upscale pixel shader (render/draw.cpp kGameUpscalePs). The game's vertex shader and draw stay; only the
// pixel work is ours. Not game code.
//
// Credits:
//  - The shader: "lanczos2" by metallic77 (github.com/metallic77), libretro glsl-shaders, windowed/shaders/lanczos2.glsl
//    (github.com/libretro/glsl-shaders, pull request #294, 2023-09-12; the collection is distributed as GPL-3.0, the
//    file itself carries no licence header). Lanczos-2 with the 4x4 footprint read in 5 fetches: one bilinear fetch
//    for the middle 2x2 and four for the edges (the corners dropped).
//  - The kernel's cubic approximations (the 1.5672 / -2.6445 / ... coefficients, no sin per pixel): going-digital
//    (github.com/going-digital), the Chebyshev fit calculator at
//    gist.github.com/going-digital/752271db735a07da7617079482394543 (no licence stated there).
// 2026-10-25, fixed ratio: 1024x576 -> 1280x720 only (the boot menu's upscaler; at 1280x720 rendering it is off). Each
// output pixel then lands on one of five positions between the source texels on each axis (distances 0.9, 0.7, 0.5,
// 0.3, 0.1 from the texel centre before it), so the kernel's weights per axis are the five rows of kPhase, computed
// with the original's cubic approximations (and its bilinear placement of the middle fetch): the same picture as the
// per-pixel version, without its polynomials.
// Adapted for TW2DE: the game's interfaces (texture coordinate at location 0, the texture of fetch constant 0 from
// the shared constants, Renut's linear clamp sampler at sampler slot 0) and alpha filtered like the colour (the
// original writes 1). The arithmetic is the original's.
#extension GL_ARB_bindless_texture : require

// The draw's pixel shader constants: c0.xy the source picture's size in texels and c1.xy one texel, as the game's
// upscale draw sets them (its texture coordinate times c0 is the position in texels).
layout(binding = 1, std140) uniform RenutUboPixel { vec4 v[257]; } g_UboPixel;
// SharedConstants (render/draw.cpp): word 0 is the 2D texture of fetch constant 0 (its descriptor in bits 0-19).
layout(binding = 2, std140) uniform RenutUboShared { uvec4 v[85]; } g_UboShared;

layout(location = 0) in vec4 iTexCoord0;
layout(location = 0) out vec4 oC0;

// tw2de (2026-11-07, dynamic resolution; render/dynamic_resolution.h): the drawn fraction of a texture resolved from a
// dynamic-resolution target (its descriptor index bits 20-25: 0 none, n 1 - (n - 1) x SharedConstants' drs_step at
// byte 1356); 1 for the rest.
float DrsFill(uint word)
{
    const uint code = (word >> 20u) & 63u;
    return code == 0u ? 1.0 : 1.0 - float(code - 1u) * uintBitsToFloat(g_UboShared.v[84].w);
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
    sampler2D src = sampler2D(uvec2(g_UboShared.v[0].x & 0xFFFFFu, 0u));
    // tw2de (2026-11-07): any source size (the texture's: 1280x720 or 1600x900 to 1920x1080 docked too), and the
    // kernel's weights computed from each pixel's fraction (exact Lanczos-2) instead of the five rows of the fixed
    // 1024x576 -> 1280x720 ratio.
    const vec2 size = vec2(textureSize(src, 0));  // the picture (the draw's c0 is stale with a 1280x720 scene)
    const vec2 inv = 1.0 / size;
    // tw2de (2026-11-07): the part drawn (dynamic resolution), and the taps kept inside it.
    const float fill = DrsFill(g_UboShared.v[0].x);
    const vec2 drawn = size * fill;
    const vec2 last = (drawn - 0.5) * inv;

    // Source position in texels, and the texel centre below-left of it.
    const vec2 src_pos = iTexCoord0.xy * drawn;
    const vec2 src_centre = floor(src_pos - 0.5) + 0.5;
    const vec2 f = src_pos - src_centre;
    // Per axis: x = the inner pair's weight (L(f) + L(1 - f)), y = the middle fetch's offset from the centre,
    // z = the outer tap before the pair (L(1 + f)), w = the one after it (L(2 - f)).
    const vec4 px = LanczosAxis(f.x);
    const vec4 py = LanczosAxis(f.y);

    const vec2 w12 = vec2(px.x, py.x);
    vec4 wedge = vec4(px.z * w12.y, py.z * w12.x, px.w * w12.y, py.w * w12.x);

    const vec2 tc12 = min(inv * (src_centre + vec2(px.y, py.y)), last);
    const vec2 tc0 = min(inv * (src_centre - 1.0), last);
    const vec2 tc3 = min(inv * (src_centre + 2.0), last);

    const float sum = wedge.x + wedge.y + wedge.z + wedge.w + w12.x * w12.y;
    wedge /= sum;
    // tw2de (2026-11-08): the middle fetch's weight divided by the sum too (it was not: the total weight went
    // 1.000-1.026 with the pixel's phase, a faint grid repeating every 6 pixels at 1600x900 -> 1920x1080).
    const float centre = w12.x * w12.y / sum;

    oC0 = texture(src, vec2(tc12.x, tc0.y)) * wedge.y +
          texture(src, vec2(tc0.x, tc12.y)) * wedge.x +
          texture(src, tc12) * centre +
          texture(src, vec2(tc3.x, tc12.y)) * wedge.z +
          texture(src, vec2(tc12.x, tc3.y)) * wedge.w;
}

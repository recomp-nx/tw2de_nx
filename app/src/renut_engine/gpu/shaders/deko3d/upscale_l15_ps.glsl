#version 460
// tw2de-dk ubo=0x4 ssbo=0x0 tex=1
// tw2de (2026-10-25, tw2de_renut_upscale_filter 2): Renut's upscaler with the game's own filter shape, drawn
// instead of the game's upscale pixel shader (render/draw.cpp kGameUpscalePs). Written for TW2DE; not game code.
//
// The filter: Lanczos with a 1.5-texel window, 3x3 texels around the sample, the weights normalised:
//   L(x) = 1.5 * sin(pi x) * sin(pi x / 1.5) / (pi x)^2 for |x| < 1.5, else 0.
// (What the game's upscale computes with sines: its taps at texel centres floor(p) + 0.5 + {-1, 0, 1}, the window 1.5,
// the weights divided by their sum.) Here the three weights of each axis are polynomials of the sample's fraction f
// in place of the sines: L(-0.5 - f) and L(0.5 - f) fitted by degree-6 Chebyshev fits on [0, 1] (numpy), the third
// L(1.5 - f) = L(-0.5 - (1 - f)) by symmetry. Normalised weights within 0.00023 of the sine kernel's on each axis
// (float32 evaluation): at most 0.11 of one 8-bit step on the output. The method (a polynomial fit of the Lanczos
// kernel so a shader needs no sines) after going-digital (gist.github.com/going-digital/752271db735a07da7617079482394543);
// the coefficients were our own fit of this kernel. 2026-10-25: fixed to 1024x576 -> 1280x720: the sample's fraction on each
// axis is one of 0, 0.2, 0.4, 0.6, 0.8, so each axis's three weights are a row of kPhase, the sine kernel's own values
// normalised (computed once, in place of the fits); the two taps of the same sign
// fetched as one bilinear sample (4 fetches in 2D).
#extension GL_ARB_bindless_texture : require

// SharedConstants (render/draw.cpp): word 0 is the 2D texture of fetch constant 0 (its descriptor in bits 0-19).
layout(binding = 2, std140) uniform RenutUboShared { uvec4 v[85]; } g_UboShared;

layout(location = 0) in vec4 iTexCoord0;
layout(location = 0) out vec4 oC0;

void main()
{
    // Renut's linear clamp sampler (slot 0): the clamp repeats the edge texels as the game's clamped sampler does.
    sampler2D src = sampler2D(uvec2(g_UboShared.v[0].x & 0xFFFFFu, 0u));
    const vec2 size = vec2(1024.0, 576.0);
    const vec2 pos = iTexCoord0.xy * size;
    const vec2 cell = floor(pos);
    const vec2 f = pos - cell;
    const vec2 centre = (cell + 0.5) * vec2(1.0 / 1024.0, 1.0 / 576.0);  // the middle tap
    // The row of each axis: fraction 0, 0.2, 0.4, 0.6, 0.8 -> 0..4.
    const ivec2 phase = clamp(ivec2(f * 5.0 + 0.5), ivec2(0), ivec2(4));
    // Per row, the three taps (-1, 0, +1; the sine kernel's own weights, normalised, computed once) as two: the two
    // of the same sign as one bilinear fetch (x: its offset from the middle tap in texels, y: its weight) and the
    // third (z: its offset, w: its weight). In 2D, 4 fetches instead of 9 (the lanczos2_ps method).
    const vec4 kPhase[5] = vec4[5](
    vec4(-0.500000000, 1.000000000, 1.0, 0.000000000),
    vec4(-0.237090901, 1.028927300, 1.0, -0.028927300),
    vec4(-0.053454456, 1.028765170, 1.0, -0.028765170),
    vec4(0.053454456, 1.028765170, -1.0, -0.028765170),
    vec4(0.237090901, 1.028927300, -1.0, -0.028927300));
    const vec4 px = kPhase[phase.x];
    const vec4 py = kPhase[phase.y];
    const vec2 inv = vec2(1.0 / 1024.0, 1.0 / 576.0);
    const vec2 pp = centre + vec2(px.x, py.x) * inv;  // the pairs
    const vec2 nn = centre + vec2(px.z, py.z) * inv;  // the thirds
    oC0 = texture(src, pp) * (px.y * py.y) + texture(src, vec2(nn.x, pp.y)) * (px.w * py.y) +
          texture(src, vec2(pp.x, nn.y)) * (px.y * py.w) + texture(src, nn) * (px.w * py.w);
}

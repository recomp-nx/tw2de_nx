#version 460
// tw2de-dk ubo=0x4 ssbo=0x0 tex=1
// tw2de (2026-10-25, tw2de_renut_upscale_filter 4, the boot menu's "Upscaler: Off"): the plainest upscale, drawn
// instead of the game's upscale pixel shader (render/draw.cpp kGameUpscalePs): one bilinear fetch a pixel, at any source
// and output size (the draw's texture coordinate). At 1280x720 rendering the draw is 1:1 and this is a copy (every
// fetch at a texel centre). Written for TW2DE; not game code.
#extension GL_ARB_bindless_texture : require

// SharedConstants (render/draw.cpp): word 0 is the 2D texture of fetch constant 0 (its descriptor in bits 0-19); with
// Renut's linear clamp sampler (slot 0).
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

void main()
{
    sampler2D src = sampler2D(uvec2(g_UboShared.v[0].x & 0xFFFFFu, 0u));
    const float fill = DrsFill(g_UboShared.v[0].x);  // tw2de (2026-11-07): dynamic resolution's drawn part
    oC0 = texture(src, min(iTexCoord0.xy * fill, vec2(fill) - 0.5 / vec2(textureSize(src, 0))));
}

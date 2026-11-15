#version 460
// tw2de-dk ubo=0x20 ssbo=0x0 tex=1
// TW2 (2026-11-12, render/draw.cpp HalfDepthFillLocked): a depth target filled from another depth target's depth
// plane (bindless textureIndex), one texel of it a pixel: texel = pixel x (param0, param1) (2 for TW2's
// half-size depth, as its shader 7BF017D4962F5915 takes every second one), the depth written as it is.
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

void main()
{
    ivec2 texel = ivec2(floor(gl_FragCoord.xy) * vec2(g_Push.param0, g_Push.param1));
    gl_FragDepth = texelFetch(sampler2D(uvec2(g_Push.textureIndex, 0u)), texel, 0).x;
}

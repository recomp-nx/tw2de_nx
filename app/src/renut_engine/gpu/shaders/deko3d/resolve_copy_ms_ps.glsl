#version 460
// tw2de-dk ubo=0x20 ssbo=0x0 tex=1
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


layout(location = 0) out vec4 out_var_SV_Target0;

void main()
{
    ivec2 _51 = ivec2(gl_FragCoord.xy) + ivec2(g_Push.translate);
    int _62 = max(int(g_Push.scale.x + 0.5), 2);
    uint _71 = 2u * (uint(max((int(g_Push.scale.y + 0.5) - 1), 0)) & 7u);
    uint _9 = g_Push.textureIndex;
    out_var_SV_Target0 = texelFetch(sampler2D(uvec2(uint(_9), 0u)), ivec3((_51.x << (((_62 >= 8) ? 2 : 1) & 31)) + int((1347481156u >> (_71 & 31u)) & 3u), (_51.y << ((int(_62 >= 4)) & 31)) + int((1347481156u >> ((16u + _71) & 31u)) & 3u), 0).xy, 0);
}


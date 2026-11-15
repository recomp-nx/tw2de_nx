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
    uint _8 = g_Push.textureIndex;
    vec4 _47 = texelFetch(sampler2D(uvec2(uint(_8), 0u)), ivec3(ivec2(gl_FragCoord.xy) + ivec2(g_Push.translate), 0).xy, 0);
    out_var_SV_Target0 = vec4(_47.xyz, _47.w * g_Push.param0);
}


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
    ivec2 _48 = ivec2(gl_FragCoord.xy) + ivec2(g_Push.translate);
    uint _9 = g_Push.textureIndex;
    vec4 _55 = texelFetch(sampler2D(uvec2(uint(_9), 0u)), ivec3(_48, 0).xy, 0);
    float _56 = _55.x;
    float _68 = _56 + (((1.0 - _56) * texelFetch(sampler2D(uvec2(uint(_9), 0u)), ivec3(max((_48 - ivec2(1)), ivec2(0)), 0).xy, 0).x) * 0.800000011920928955078125);
    float _74;
    if (_68 > 0.0)
    {
        _74 = _56 / _68;
    }
    else
    {
        _74 = 0.0;
    }
    out_var_SV_Target0 = vec4(_74, _74, _74, _68 * g_Push.param0);
}


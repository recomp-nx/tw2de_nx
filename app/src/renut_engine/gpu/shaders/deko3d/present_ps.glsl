#version 460
// tw2de-dk ubo=0x20 ssbo=0x0 tex=1
#extension GL_ARB_bindless_texture : require
#define dkh(t, s) uvec2(uint(t) | (uint(s) << 20u), 0u)

vec4 _58;

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

void main()
{
    uint _17 = g_Push.textureIndex;
    uint _20 = g_Push.samplerIndex;
    vec4 _65 = texture((sampler2D(dkh(_17, _20))), in_var_TEXCOORD0);
    vec3 _71 = pow(clamp(_65.xyz, vec3(0.0), vec3(1.0)), vec3(g_Push.param0));
    uint _75 = floatBitsToUint(g_Push.param1);
    vec4 _102;
    if (_75 != 0u)
    {
        ivec3 _83 = ivec3(roundEven(clamp(_71.xyz, vec3(0.0), vec3(1.0)) * 255.0));
        uint _12 = _75;
        vec4 _89;
        _89.x = texelFetch(sampler2D(uvec2(uint(_12), 0u)), ivec3(_83.x, 0, 0).xy, 0).x;
        _89.y = texelFetch(sampler2D(uvec2(uint(_12), 0u)), ivec3(_83.y, 0, 0).xy, 0).y;
        _89.z = texelFetch(sampler2D(uvec2(uint(_12), 0u)), ivec3(_83.z, 0, 0).xy, 0).z;
        _102 = _89;
    }
    else
    {
        _102 = vec4(_71.x, _71.y, _71.z, _65.w);
    }
    out_var_SV_Target0 = vec4(_102.xyz, 1.0);
}


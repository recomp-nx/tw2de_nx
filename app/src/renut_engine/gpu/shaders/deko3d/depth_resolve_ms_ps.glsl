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


layout(location = 0) out float out_var_SV_Target0;
layout(location = 1) out vec4 out_var_SV_Target1;

void main()
{
    ivec2 _78 = ivec2(gl_FragCoord.xy) + ivec2(g_Push.translate);
    uint _12 = g_Push.textureIndex;
    int _91 = max(int(g_Push.scale.x + 0.5), 2);
    uint _100 = 2u * (uint(max((int(g_Push.scale.y + 0.5) - 1), 0)) & 7u);
    ivec2 _119 = ivec3((_78.x << (((_91 >= 8) ? 2 : 1) & 31)) + int((1347481156u >> (_100 & 31u)) & 3u), (_78.y << ((int(_91 >= 4)) & 31)) + int((1347481156u >> ((16u + _100) & 31u)) & 3u), 0).xy;
    vec4 _122 = texelFetch(sampler2D(uvec2(uint(_12), 0u)), _119, 0);
    float _123 = _122.x;
    uint _134;
    if (g_Push.param1 != 0.0)
    {
        uint _13 = g_Push.samplerIndex;
        _134 = texelFetch(usampler2D(uvec2(uint(_13), 0u)), _119, 0).x & 255u;
    }
    else
    {
        _134 = 0u;
    }
    uint _161;
    if (g_Push.param0 != 0.0)
    {
        uint _156;
        do
        {
            float _143 = clamp(_123, 0.0, 1.99999988079071044921875);
            if (_143 < 6.103515625e-05)
            {
                _156 = uint(_143 * 17179869184.0);
                break;
            }
            uint _149 = floatBitsToUint(_143);
            _156 = (((_149 >> 23u) - 112u) << 20u) | ((_149 >> 3u) & 1048575u);
            break;
        } while(false);
        _161 = _156;
    }
    else
    {
        _161 = uint((clamp(_123, 0.0, 1.0) * 16777215.0) + 0.5);
    }
    uint _163 = (_161 << 8u) | _134;
    out_var_SV_Target0 = _123;
    out_var_SV_Target1 = vec4(float(_163 & 255u), float((_163 >> 8u) & 255u), float((_163 >> 16u) & 255u), float(_163 >> 24u)) * vec4(0.0039215688593685626983642578125);
}


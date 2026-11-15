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
    uint _12 = g_Push.textureIndex;
    ivec2 _78 = ivec3(ivec2(gl_FragCoord.xy) + ivec2(g_Push.translate), 0).xy;
    vec4 _81 = texelFetch(sampler2D(uvec2(uint(_12), 0u)), _78, 0);
    float _82 = _81.x;
    uint _93;
    if (g_Push.param1 != 0.0)
    {
        uint _13 = g_Push.samplerIndex;
        _93 = texelFetch(usampler2D(uvec2(uint(_13), 0u)), _78, 0).x & 255u;
    }
    else
    {
        _93 = 0u;
    }
    uint _120;
    if (g_Push.param0 != 0.0)
    {
        uint _115;
        do
        {
            float _102 = clamp(_82, 0.0, 1.99999988079071044921875);
            if (_102 < 6.103515625e-05)
            {
                _115 = uint(_102 * 17179869184.0);
                break;
            }
            uint _108 = floatBitsToUint(_102);
            _115 = (((_108 >> 23u) - 112u) << 20u) | ((_108 >> 3u) & 1048575u);
            break;
        } while(false);
        _120 = _115;
    }
    else
    {
        _120 = uint((clamp(_82, 0.0, 1.0) * 16777215.0) + 0.5);
    }
    uint _122 = (_120 << 8u) | _93;
    out_var_SV_Target0 = _82;
    out_var_SV_Target1 = vec4(float(_122 & 255u), float((_122 >> 8u) & 255u), float((_122 >> 16u) & 255u), float(_122 >> 24u)) * vec4(0.0039215688593685626983642578125);
}


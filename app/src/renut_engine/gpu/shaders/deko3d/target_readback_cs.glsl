#version 460
// tw2de-dk ubo=0x20 ssbo=0x1 tex=1
#extension GL_ARB_bindless_texture : require
#define dkh(t, s) uvec2(uint(t) | (uint(s) << 20u), 0u)
#if defined(GL_EXT_control_flow_attributes)
#extension GL_EXT_control_flow_attributes : require
#define SPIRV_CROSS_FLATTEN [[flatten]]
#define SPIRV_CROSS_BRANCH [[dont_flatten]]
#define SPIRV_CROSS_UNROLL [[unroll]]
#define SPIRV_CROSS_LOOP [[dont_unroll]]
#else
#define SPIRV_CROSS_FLATTEN
#define SPIRV_CROSS_BRANCH
#define SPIRV_CROSS_UNROLL
#define SPIRV_CROSS_LOOP
#endif
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0, std430) buffer type_RWByteAddressBuffer
{
    uint _m0[];
} g_Words;

layout(std140, binding = 5) uniform type_ConstantBuffer_TargetReadbackPush
{
    uint textureIndex;
    uint width;
    uint height;
    uint scaleNumerator;
    uint scaleDenominator;
    uint pad0;
    uint pad1;
    uint pad2;
} g_Push;


void main()
{
    do
    {
        bool _75;
        if (!(gl_GlobalInvocationID.x >= g_Push.width))
        {
            _75 = gl_GlobalInvocationID.y >= g_Push.height;
        }
        else
        {
            _75 = true;
        }
        if (_75)
        {
            break;
        }
        uint _11 = g_Push.textureIndex;
        uvec2 _80 = uvec2(textureSize(sampler2D(uvec2(uint(_11), 0u)), 0));
        uint _88 = ((gl_GlobalInvocationID.x * g_Push.scaleNumerator) * 2u) + g_Push.scaleDenominator;
        uint _89 = g_Push.scaleDenominator * 2u;
        float _91 = float(_89);
        uint _94 = uint((float(_88) / _91) * 0.999989986419677734375);
        uint _101 = _94 + uint((float(_88 - (_94 * _89)) / _91) * 0.999989986419677734375);
        uint _103 = _88 - (_101 * _89);
        uint _105;
        _105 = _101;
        SPIRV_CROSS_LOOP
        for (uint _108 = _103; _108 >= _89; )
        {
            _105++;
            _108 -= _89;
            continue;
        }
        uint _116 = ((gl_GlobalInvocationID.y * g_Push.scaleNumerator) * 2u) + g_Push.scaleDenominator;
        uint _120 = uint((float(_116) / _91) * 0.999989986419677734375);
        uint _127 = _120 + uint((float(_116 - (_120 * _89)) / _91) * 0.999989986419677734375);
        uint _129 = _116 - (_127 * _89);
        uint _131;
        _131 = _127;
        SPIRV_CROSS_LOOP
        for (uint _134 = _129; _134 >= _89; )
        {
            _131++;
            _134 -= _89;
            continue;
        }
        uvec2 _139 = min(uvec2(_105, _131), (_80 - uvec2(1u)));
        uint _143 = (((gl_GlobalInvocationID.x + 1u) * g_Push.scaleNumerator) * 2u) + g_Push.scaleDenominator;
        uint _147 = uint((float(_143) / _91) * 0.999989986419677734375);
        uint _154 = _147 + uint((float(_143 - (_147 * _89)) / _91) * 0.999989986419677734375);
        uint _156 = _143 - (_154 * _89);
        uint _158;
        _158 = _154;
        SPIRV_CROSS_LOOP
        for (uint _161 = _156; _161 >= _89; )
        {
            _158++;
            _161 -= _89;
            continue;
        }
        uint _168 = (((gl_GlobalInvocationID.y + 1u) * g_Push.scaleNumerator) * 2u) + g_Push.scaleDenominator;
        uint _172 = uint((float(_168) / _91) * 0.999989986419677734375);
        uint _179 = _172 + uint((float(_168 - (_172 * _89)) / _91) * 0.999989986419677734375);
        uint _181 = _168 - (_179 * _89);
        uint _183;
        _183 = _179;
        SPIRV_CROSS_LOOP
        for (uint _186 = _181; _186 >= _89; )
        {
            _183++;
            _186 -= _89;
            continue;
        }
        uvec2 _192 = clamp(uvec2(_158, _183), _139 + uvec2(1u), _80);
        uint _193 = _139.y;
        vec4 _195;
        uint _198;
        _195 = vec4(0.0);
        _198 = _193;
        vec4 _196;
        uint _200;
        for (;;)
        {
            _200 = _192.y;
            bool _201 = _198 < _200;
            if (_201)
            {
                uint _204 = _139.x;
                _196 = _195;
                for (uint _208 = _204; _208 < _192.x; )
                {
                    _196 += texelFetch(sampler2D(uvec2(uint(_11), 0u)), ivec3(int(_208), int(_198), 0).xy, 0);
                    _208++;
                    continue;
                }
                _195 = _196;
                _198++;
                continue;
            }
            else
            {
                break;
            }
        }
        uvec3 _230 = uvec3((clamp(_195.xyz / vec3(float((_192.x - _139.x) * (_200 - _193))), vec3(0.0), vec3(1.0)) * 255.0) + vec3(0.5));
        g_Words._m0[(((gl_GlobalInvocationID.y * g_Push.width) + gl_GlobalInvocationID.x) * 4u) >> 2u] = ((255u | (_230.x << 8u)) | (_230.y << 16u)) | (_230.z << 24u);
        break;
    } while(false);
}


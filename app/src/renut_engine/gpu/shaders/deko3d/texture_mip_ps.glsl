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
    uint _9 = g_Push.textureIndex;
    uvec2 _42 = uvec2(textureSize(sampler2D(uvec2(uint(_9), 0u)), 0));
    int _47 = int(g_Push.param0);
    ivec2 _51 = ivec2(gl_FragCoord.xy) * ivec2(_47);
    ivec2 _55 = ivec2(int(_42.x), int(_42.y)) - ivec2(1);
    vec4 _57;
    int _60;
    _57 = vec4(0.0);
    _60 = 0;
    vec4 _58;
    for (; _60 < _47; _57 = _58, _60++)
    {
        _58 = _57;
        for (int _68 = 0; _68 < _47; )
        {
            _58 += texelFetch(sampler2D(uvec2(uint(_9), 0u)), ivec3(min((_51 + ivec2(_68, _60)), _55), 0).xy, 0);
            _68++;
            continue;
        }
    }
    out_var_SV_Target0 = _57 / vec4(float(_47 * _47));
}


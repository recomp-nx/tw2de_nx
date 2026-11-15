#version 460
// tw2de-dk ubo=0x20 ssbo=0x0 tex=0

layout(std140, binding = 5) uniform type_ConstantBuffer_UtilityPush
{
    vec2 scale;
    vec2 translate;
    uint textureIndex;
    uint samplerIndex;
    float param0;
    float param1;
} g_Push;

layout(location = 0) out vec2 out_var_TEXCOORD0;

void main()
{
    vec2 _37 = vec2(float((uint(gl_VertexID) << 1u) & 2u), float(uint(gl_VertexID) & 2u));
    gl_Position = vec4((_37 * vec2(2.0, -2.0)) + vec2(-1.0, 1.0), 0.0, 1.0);
    out_var_TEXCOORD0 = (_37 * g_Push.scale) + g_Push.translate;
}


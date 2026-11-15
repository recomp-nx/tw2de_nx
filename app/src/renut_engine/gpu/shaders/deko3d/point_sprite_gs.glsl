#version 460
// tw2de-dk ubo=0x4 ssbo=0x0 tex=0
layout(points) in;
layout(max_vertices = 4, triangle_strip) out;

layout(binding = 2, std140) uniform type_ConstantBuffer_FNXF_XIII_LRSharedConstants
{
    vec4 v[84];
} g_FNXF_XIII_LRShared;

layout(location = 0) in vec4 in_var_TEXCOORD0[1][16];
layout(location = 16) in vec4 in_var_COLOR0[1][2];
layout(location = 18) in vec4 in_var_NORMAL0[1][3];
layout(location = 21) in vec4 in_var_TANGENT0[1];
layout(location = 22) in vec4 in_var_BINORMAL0[1];
layout(location = 24) in float in_var_POINTSIZE[1];
// tw2de (2026-10-27): one output per location (plume reads the locations from these lines: constant 905).
layout(location = 0) out vec4 out_var_TEXCOORD0;
layout(location = 1) out vec4 out_var_TEXCOORD1;
layout(location = 2) out vec4 out_var_TEXCOORD2;
layout(location = 3) out vec4 out_var_TEXCOORD3;
layout(location = 4) out vec4 out_var_TEXCOORD4;
layout(location = 5) out vec4 out_var_TEXCOORD5;
layout(location = 6) out vec4 out_var_TEXCOORD6;
layout(location = 7) out vec4 out_var_TEXCOORD7;
layout(location = 8) out vec4 out_var_TEXCOORD8;
layout(location = 9) out vec4 out_var_TEXCOORD9;
layout(location = 10) out vec4 out_var_TEXCOORD10;
layout(location = 11) out vec4 out_var_TEXCOORD11;
layout(location = 12) out vec4 out_var_TEXCOORD12;
layout(location = 13) out vec4 out_var_TEXCOORD13;
layout(location = 14) out vec4 out_var_TEXCOORD14;
layout(location = 15) out vec4 out_var_TEXCOORD15;
layout(location = 16) out vec4 out_var_COLOR0;
layout(location = 17) out vec4 out_var_COLOR1;
layout(location = 18) out vec4 out_var_NORMAL0;
layout(location = 19) out vec4 out_var_NORMAL1;
layout(location = 20) out vec4 out_var_NORMAL2;
layout(location = 21) out vec4 out_var_TANGENT0;
layout(location = 22) out vec4 out_var_BINORMAL0;
layout(location = 23) out vec2 out_var_POINTCOORD;
layout(location = 24) out float out_var_POINTSIZE;

// tw2de (2026-10-27): constant 905, the output locations the pixel shader reads (plume_deko3d.cpp sets it per
// pipeline; all of them by default), as rectangle_list_gs. Every point wrote all 25 varyings to each of its 4 corners
// (109 registers), the game's memexport kernels' points too (they draw nothing: their positions are all zero).
#ifndef SPIRV_CROSS_CONSTANT_ID_905
#define SPIRV_CROSS_CONSTANT_ID_905 4294967295u
#endif
const uint g_gsOutputs = SPIRV_CROSS_CONSTANT_ID_905;
#define GS_OUT(loc) ((g_gsOutputs & (1u << (loc))) != 0u)

// A corner of the sprite: at the point plus 'offset' (clip space), with its point coordinate.
void emitCorner(vec2 offset, vec2 pointCoord)
{
    vec4 p = gl_in[0].gl_Position;
    gl_Position = vec4(p.xy + offset, p.z, p.w);
    if (GS_OUT(0))
        out_var_TEXCOORD0 = in_var_TEXCOORD0[0][0];
    if (GS_OUT(1))
        out_var_TEXCOORD1 = in_var_TEXCOORD0[0][1];
    if (GS_OUT(2))
        out_var_TEXCOORD2 = in_var_TEXCOORD0[0][2];
    if (GS_OUT(3))
        out_var_TEXCOORD3 = in_var_TEXCOORD0[0][3];
    if (GS_OUT(4))
        out_var_TEXCOORD4 = in_var_TEXCOORD0[0][4];
    if (GS_OUT(5))
        out_var_TEXCOORD5 = in_var_TEXCOORD0[0][5];
    if (GS_OUT(6))
        out_var_TEXCOORD6 = in_var_TEXCOORD0[0][6];
    if (GS_OUT(7))
        out_var_TEXCOORD7 = in_var_TEXCOORD0[0][7];
    if (GS_OUT(8))
        out_var_TEXCOORD8 = in_var_TEXCOORD0[0][8];
    if (GS_OUT(9))
        out_var_TEXCOORD9 = in_var_TEXCOORD0[0][9];
    if (GS_OUT(10))
        out_var_TEXCOORD10 = in_var_TEXCOORD0[0][10];
    if (GS_OUT(11))
        out_var_TEXCOORD11 = in_var_TEXCOORD0[0][11];
    if (GS_OUT(12))
        out_var_TEXCOORD12 = in_var_TEXCOORD0[0][12];
    if (GS_OUT(13))
        out_var_TEXCOORD13 = in_var_TEXCOORD0[0][13];
    if (GS_OUT(14))
        out_var_TEXCOORD14 = in_var_TEXCOORD0[0][14];
    if (GS_OUT(15))
        out_var_TEXCOORD15 = in_var_TEXCOORD0[0][15];
    if (GS_OUT(16))
        out_var_COLOR0 = in_var_COLOR0[0][0];
    if (GS_OUT(17))
        out_var_COLOR1 = in_var_COLOR0[0][1];
    if (GS_OUT(18))
        out_var_NORMAL0 = in_var_NORMAL0[0][0];
    if (GS_OUT(19))
        out_var_NORMAL1 = in_var_NORMAL0[0][1];
    if (GS_OUT(20))
        out_var_NORMAL2 = in_var_NORMAL0[0][2];
    if (GS_OUT(21))
        out_var_TANGENT0 = in_var_TANGENT0[0];
    if (GS_OUT(22))
        out_var_BINORMAL0 = in_var_BINORMAL0[0];
    if (GS_OUT(23))
        out_var_POINTCOORD = pointCoord;
    if (GS_OUT(24))
        out_var_POINTSIZE = in_var_POINTSIZE[0];
    EmitVertex();
}

void main()
{
    // A negative size: no sprite. 0: the render state's size (SharedConstants vector 82: x y, z min, w max).
    float size = in_var_POINTSIZE[0];
    if (size < 0.0)
        return;
    vec2 s = size > 0.0 ? vec2(size) : g_FNXF_XIII_LRShared.v[82].xy;
    if (g_FNXF_XIII_LRShared.v[82].w > 0.0)
        s = min(s, vec2(g_FNXF_XIII_LRShared.v[82].w));
    vec2 half_ = (max(s, vec2(g_FNXF_XIII_LRShared.v[82].z)) / g_FNXF_XIII_LRShared.v[83].xy) * gl_in[0].gl_Position.w;
    emitCorner(vec2(-1.0, 1.0) * half_, vec2(0.0, 0.0));
    emitCorner(half_, vec2(1.0, 0.0));
    emitCorner(vec2(-1.0) * half_, vec2(0.0, 1.0));
    emitCorner(vec2(1.0, -1.0) * half_, vec2(1.0, 1.0));
}

#version 460
// tw2de-dk ubo=0x0 ssbo=0x0 tex=0
layout(triangles) in;
layout(max_vertices = 4, triangle_strip) out;

layout(location = 0) in vec4 in_var_TEXCOORD0[3][16];
layout(location = 16) in vec4 in_var_COLOR0[3][2];
layout(location = 18) in vec4 in_var_NORMAL0[3][3];
layout(location = 21) in vec4 in_var_TANGENT0[3];
layout(location = 22) in vec4 in_var_BINORMAL0[3];
layout(location = 23) in vec2 in_var_POINTCOORD[3];
layout(location = 24) in float in_var_POINTSIZE[3];
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
// pipeline, as for a vertex shader without a geometry shader; all of them by default). A geometry shader holds every
// output of a vertex in registers until EmitVertex: the 25 varyings took 108 registers, the ones a pipeline's pixel
// shader reads (a few texture coordinates and a colour) far fewer.
#ifndef SPIRV_CROSS_CONSTANT_ID_905
#define SPIRV_CROSS_CONSTANT_ID_905 4294967295u
#endif
const uint g_gsOutputs = SPIRV_CROSS_CONSTANT_ID_905;
#define GS_OUT(loc) ((g_gsOutputs & (1u << (loc))) != 0u)

// tw2de (2026-10-27): the inputs read straight from the input arrays, no local copy. SPIRV-Cross copied all three
// vertices into a local array of structs indexed by the corner picked at run time: 52 KB of scratch a warp and 255
// registers (uam). A corner as it is, by its index; the fourth by constant indices, one of three copies (computing it
// with run-time indices still took 6 KB of scratch and 255 registers).
void emitCorner(uint v)
{
    gl_Position = gl_in[v].gl_Position;
    if (GS_OUT(0))
        out_var_TEXCOORD0 = in_var_TEXCOORD0[v][0];
    if (GS_OUT(1))
        out_var_TEXCOORD1 = in_var_TEXCOORD0[v][1];
    if (GS_OUT(2))
        out_var_TEXCOORD2 = in_var_TEXCOORD0[v][2];
    if (GS_OUT(3))
        out_var_TEXCOORD3 = in_var_TEXCOORD0[v][3];
    if (GS_OUT(4))
        out_var_TEXCOORD4 = in_var_TEXCOORD0[v][4];
    if (GS_OUT(5))
        out_var_TEXCOORD5 = in_var_TEXCOORD0[v][5];
    if (GS_OUT(6))
        out_var_TEXCOORD6 = in_var_TEXCOORD0[v][6];
    if (GS_OUT(7))
        out_var_TEXCOORD7 = in_var_TEXCOORD0[v][7];
    if (GS_OUT(8))
        out_var_TEXCOORD8 = in_var_TEXCOORD0[v][8];
    if (GS_OUT(9))
        out_var_TEXCOORD9 = in_var_TEXCOORD0[v][9];
    if (GS_OUT(10))
        out_var_TEXCOORD10 = in_var_TEXCOORD0[v][10];
    if (GS_OUT(11))
        out_var_TEXCOORD11 = in_var_TEXCOORD0[v][11];
    if (GS_OUT(12))
        out_var_TEXCOORD12 = in_var_TEXCOORD0[v][12];
    if (GS_OUT(13))
        out_var_TEXCOORD13 = in_var_TEXCOORD0[v][13];
    if (GS_OUT(14))
        out_var_TEXCOORD14 = in_var_TEXCOORD0[v][14];
    if (GS_OUT(15))
        out_var_TEXCOORD15 = in_var_TEXCOORD0[v][15];
    if (GS_OUT(16))
        out_var_COLOR0 = in_var_COLOR0[v][0];
    if (GS_OUT(17))
        out_var_COLOR1 = in_var_COLOR0[v][1];
    if (GS_OUT(18))
        out_var_NORMAL0 = in_var_NORMAL0[v][0];
    if (GS_OUT(19))
        out_var_NORMAL1 = in_var_NORMAL0[v][1];
    if (GS_OUT(20))
        out_var_NORMAL2 = in_var_NORMAL0[v][2];
    if (GS_OUT(21))
        out_var_TANGENT0 = in_var_TANGENT0[v];
    if (GS_OUT(22))
        out_var_BINORMAL0 = in_var_BINORMAL0[v];
    if (GS_OUT(23))
        out_var_POINTCOORD = in_var_POINTCOORD[v];
    if (GS_OUT(24))
        out_var_POINTSIZE = in_var_POINTSIZE[v];
    EmitVertex();
}

// The fourth corner, a + b - c (constant indices).
void emitFourth(const int c, const int a, const int b)
{
    gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;
    if (GS_OUT(0))
        out_var_TEXCOORD0 = in_var_TEXCOORD0[a][0] + in_var_TEXCOORD0[b][0] - in_var_TEXCOORD0[c][0];
    if (GS_OUT(1))
        out_var_TEXCOORD1 = in_var_TEXCOORD0[a][1] + in_var_TEXCOORD0[b][1] - in_var_TEXCOORD0[c][1];
    if (GS_OUT(2))
        out_var_TEXCOORD2 = in_var_TEXCOORD0[a][2] + in_var_TEXCOORD0[b][2] - in_var_TEXCOORD0[c][2];
    if (GS_OUT(3))
        out_var_TEXCOORD3 = in_var_TEXCOORD0[a][3] + in_var_TEXCOORD0[b][3] - in_var_TEXCOORD0[c][3];
    if (GS_OUT(4))
        out_var_TEXCOORD4 = in_var_TEXCOORD0[a][4] + in_var_TEXCOORD0[b][4] - in_var_TEXCOORD0[c][4];
    if (GS_OUT(5))
        out_var_TEXCOORD5 = in_var_TEXCOORD0[a][5] + in_var_TEXCOORD0[b][5] - in_var_TEXCOORD0[c][5];
    if (GS_OUT(6))
        out_var_TEXCOORD6 = in_var_TEXCOORD0[a][6] + in_var_TEXCOORD0[b][6] - in_var_TEXCOORD0[c][6];
    if (GS_OUT(7))
        out_var_TEXCOORD7 = in_var_TEXCOORD0[a][7] + in_var_TEXCOORD0[b][7] - in_var_TEXCOORD0[c][7];
    if (GS_OUT(8))
        out_var_TEXCOORD8 = in_var_TEXCOORD0[a][8] + in_var_TEXCOORD0[b][8] - in_var_TEXCOORD0[c][8];
    if (GS_OUT(9))
        out_var_TEXCOORD9 = in_var_TEXCOORD0[a][9] + in_var_TEXCOORD0[b][9] - in_var_TEXCOORD0[c][9];
    if (GS_OUT(10))
        out_var_TEXCOORD10 = in_var_TEXCOORD0[a][10] + in_var_TEXCOORD0[b][10] - in_var_TEXCOORD0[c][10];
    if (GS_OUT(11))
        out_var_TEXCOORD11 = in_var_TEXCOORD0[a][11] + in_var_TEXCOORD0[b][11] - in_var_TEXCOORD0[c][11];
    if (GS_OUT(12))
        out_var_TEXCOORD12 = in_var_TEXCOORD0[a][12] + in_var_TEXCOORD0[b][12] - in_var_TEXCOORD0[c][12];
    if (GS_OUT(13))
        out_var_TEXCOORD13 = in_var_TEXCOORD0[a][13] + in_var_TEXCOORD0[b][13] - in_var_TEXCOORD0[c][13];
    if (GS_OUT(14))
        out_var_TEXCOORD14 = in_var_TEXCOORD0[a][14] + in_var_TEXCOORD0[b][14] - in_var_TEXCOORD0[c][14];
    if (GS_OUT(15))
        out_var_TEXCOORD15 = in_var_TEXCOORD0[a][15] + in_var_TEXCOORD0[b][15] - in_var_TEXCOORD0[c][15];
    if (GS_OUT(16))
        out_var_COLOR0 = in_var_COLOR0[a][0] + in_var_COLOR0[b][0] - in_var_COLOR0[c][0];
    if (GS_OUT(17))
        out_var_COLOR1 = in_var_COLOR0[a][1] + in_var_COLOR0[b][1] - in_var_COLOR0[c][1];
    if (GS_OUT(18))
        out_var_NORMAL0 = in_var_NORMAL0[a][0] + in_var_NORMAL0[b][0] - in_var_NORMAL0[c][0];
    if (GS_OUT(19))
        out_var_NORMAL1 = in_var_NORMAL0[a][1] + in_var_NORMAL0[b][1] - in_var_NORMAL0[c][1];
    if (GS_OUT(20))
        out_var_NORMAL2 = in_var_NORMAL0[a][2] + in_var_NORMAL0[b][2] - in_var_NORMAL0[c][2];
    if (GS_OUT(21))
        out_var_TANGENT0 = in_var_TANGENT0[a] + in_var_TANGENT0[b] - in_var_TANGENT0[c];
    if (GS_OUT(22))
        out_var_BINORMAL0 = in_var_BINORMAL0[a] + in_var_BINORMAL0[b] - in_var_BINORMAL0[c];
    if (GS_OUT(23))
        out_var_POINTCOORD = in_var_POINTCOORD[a] + in_var_POINTCOORD[b] - in_var_POINTCOORD[c];
    if (GS_OUT(24))
        out_var_POINTSIZE = in_var_POINTSIZE[a] + in_var_POINTSIZE[b] - in_var_POINTSIZE[c];
    EmitVertex();
}

void main()
{
    // The corner with the right angle: the one where its two edges' dot product is smallest (as before: 1 over 2 on a
    // tie, 0 on a tie with either).
    vec2 p0 = gl_in[0].gl_Position.xy / vec2(gl_in[0].gl_Position.w);
    vec2 p1 = gl_in[1].gl_Position.xy / vec2(gl_in[1].gl_Position.w);
    vec2 p2 = gl_in[2].gl_Position.xy / vec2(gl_in[2].gl_Position.w);
    float d0 = abs(dot(p1 - p0, p2 - p0));
    float d1 = abs(dot(p2 - p1, p0 - p1));
    float d2 = abs(dot(p0 - p2, p1 - p2));
    uint c = (d1 < d0 && d1 <= d2) ? 1u : ((d2 < d0 && d2 < d1) ? 2u : 0u);
    uint a = (c + 1u) % 3u;
    uint b = (c + 2u) % 3u;
    emitCorner(c);
    emitCorner(a);
    emitCorner(b);
    if (c == 1u)
        emitFourth(1, 2, 0);
    else if (c == 2u)
        emitFourth(2, 0, 1);
    else
        emitFourth(0, 1, 2);
}

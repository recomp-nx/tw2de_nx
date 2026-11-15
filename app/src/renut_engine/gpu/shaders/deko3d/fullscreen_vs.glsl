#version 460
// tw2de-dk ubo=0x0 ssbo=0x0 tex=0
// tw2de (2026-10-25): a full-screen triangle (vertices 0-2, no vertex buffer), for Renut's own full-screen passes
// (render/fsr1.cpp's RCAS). The texture coordinate runs 0..1 over the screen (2 at the triangle's far corners).

layout(location = 0) out vec2 out_var_TEXCOORD0;

void main()
{
    vec2 uv = vec2(float((uint(gl_VertexID) << 1u) & 2u), float(uint(gl_VertexID) & 2u));
    gl_Position = vec4((uv * vec2(2.0, -2.0)) + vec2(-1.0, 1.0), 0.0, 1.0);
    out_var_TEXCOORD0 = uv;
}

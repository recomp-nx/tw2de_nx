#version 460
// tw2de-dk ubo=0x0 ssbo=0x0 tex=0
// tw2de (2026-11-06, render/fsr3.cpp, tw2de_fsr3_diag): one of FSR 3's textures shown as the output (a
// diagnostic): texel (x, y) of texture 0 at the output's (x, y) scaled by its size; rgb = value.xyz * scale.xyz +
// bias.xyz, or with mode 1 the motion (xy) as colour: |x|, |y| times scale.x, blue the sign.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, std140) uniform Fsr3View
{
    vec4 scale;
    vec4 bias;
    uvec4 size;  // xy: the output's size, zw: the texture's
    uvec4 mode;  // x: 0 plain, 1 motion
} g_View;
layout(binding = 0) uniform sampler2D g_Texture;
layout(binding = 0) uniform writeonly image2D g_Output;
void main()
{
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= g_View.size.x || p.y >= g_View.size.y)
        return;
    ivec2 t = ivec2((vec2(p) + 0.5) * vec2(g_View.size.zw) / vec2(g_View.size.xy));
    vec4 v = texelFetch(g_Texture, t, 0);
    vec3 c;
    if (g_View.mode.x == 1u)
        c = vec3(clamp(abs(v.xy) * g_View.scale.x, 0.0, 1.0), (v.x < 0.0 ? 0.5 : 0.0) + (v.y < 0.0 ? 0.5 : 0.0));
    else
        c = clamp(v.xyz * g_View.scale.xyz + g_View.bias.xyz, 0.0, 1.0);
    // NaN: magenta, infinity: yellow (both would otherwise show as black or white).
    if (any(isnan(v)))
        c = vec3(1.0, 0.0, 1.0);
    else if (any(isinf(v)))
        c = vec3(1.0, 1.0, 0.0);
    imageStore(g_Output, ivec2(p), vec4(c, 1.0));
}

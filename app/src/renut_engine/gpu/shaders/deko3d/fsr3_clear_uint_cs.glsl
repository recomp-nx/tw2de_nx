#version 460
// tw2de-dk ubo=0x0 ssbo=0x0 tex=0
// tw2de (2026-11-06, render/fsr3.cpp): fsr3_clear_cs for an unsigned integer image (r32ui).
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, std140) uniform Fsr3Clear
{
    uvec4 value;
    uvec4 size;
} g_Clear;
layout(binding = 0, r32ui) uniform writeonly uimage2D g_Image;
void main()
{
    if (gl_GlobalInvocationID.x < g_Clear.size.x && gl_GlobalInvocationID.y < g_Clear.size.y)
        imageStore(g_Image, ivec2(gl_GlobalInvocationID.xy), g_Clear.value);
}

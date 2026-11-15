#version 460
// tw2de-dk ubo=0x0 ssbo=0x0 tex=0
// tw2de (2026-11-06, render/fsr3.cpp): FSR 3's reconstructed previous depth buffer (storage buffer 0) set to a value,
// its first size.x words.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(binding = 0, std140) uniform Fsr3Clear
{
    uvec4 value;
    uvec4 size;
} g_Clear;
layout(binding = 0, std430) buffer Fsr3ReconDepth_t
{
    uint data[];
} g_Buffer;
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i < g_Clear.size.x)
        g_Buffer.data[i] = g_Clear.value.x;
}

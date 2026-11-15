# Renut's own shaders for the Switch (deko3d)

The renderer's 13 shaders (present, the compile notice, the achievement popup, resolves, mip generation, the
rectangle-list and point-sprite geometry shaders, the stream-convert and readback compute shaders), as the GLSL the
console compiles. The build embeds the text, and (2026-11-06) each one's DKSH made on the PC with devkitPro's uam
(`$DEVKITPRO/tools/bin/uam`: every pass, the best code) in the NRO's romfs as `shaders/<stem>.dksh`,
used as the default variant (never put in the SD card's DKSH cache). The 17 without specialization constants are then
in the NRO without their text, as the metadata plume reads from it (`app/cmake/tw2de_glsl_blob.cmake` META_ONLY;
`tools/native_tests/host_shader_meta_check.py` compares it with plume's own parser); the 2 geometry shaders with
constant 905 keep their text for their other variants. Without devkitPro's uam every one is its text and the NRO
compiles them itself at their first use, then from the DKSH cache on the SD card. No DXC or SPIRV-Cross on the PC.

`<stem>.glsl`: the stage is the stem's suffix (`_vs` vertex, `_gs` geometry, `_ps` fragment, `_cs` compute).

They were made from `../host/<stem>.hlsl` (DXC with `-DTW2DE_DEKO3D`, SPIRV-Cross 4.60, `tools/deko3d_glsl.py`). If
one of those HLSL files changes, its GLSL here has to be made again the same way (`tools/deko3d_host_shader.py` writes
it into its header as the text after the 16-byte "BDK3" header); the Switch build itself never reads the HLSL.

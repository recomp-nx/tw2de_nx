# reNut native renderer: plume (tw2de: its deko3d back end, the Switch only), the host shaders, and the game's
# shaders translated on the console (app/cmake/tw2de_renut_gpu.cmake: tw2de_renut_shader_cache).
#
# Included from app/cmake/tw2de_renut_gpu.cmake after the target exists:
#   include(src/renut_engine/gpu/gpu.cmake)
#   renut_gpu_setup(<target>)
#
# tw2de (2026-10-29): Switch only. Renut's PC parts are gone: the shader dump and the translator built on the PC
# (ExternalProject), DXC (HLSL host shaders, DXIL/SPIR-V), the packed shader cache and the D3D12/X11 links.

set(RENUT_GPU_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(RENUT_XENOS_RECOMP_DIR "${CMAKE_SOURCE_DIR}/thirdparty/reblue-XenosRecomp")
set(RENUT_SHADER_COMMON_DIR "${RENUT_XENOS_RECOMP_DIR}/XenosRecomp")
set(RENUT_GPU_GEN_DIR "${CMAKE_BINARY_DIR}/renut_gpu")

# plume is read-only and vendored; build it as-is.
if(NOT TARGET plume)
    add_subdirectory("${CMAKE_SOURCE_DIR}/thirdparty/plume" "${CMAKE_BINARY_DIR}/plume")
endif()

# The shader cache is zstd-compressed and its SPIR-V is smol-v encoded. Only the
# decoders are needed, built from the copies vendored with XenosRecomp.
enable_language(C)
set(RENUT_ZSTD_LIB_DIR "${RENUT_XENOS_RECOMP_DIR}/thirdparty/zstd/lib")
file(GLOB renut_zstd_sources
    "${RENUT_ZSTD_LIB_DIR}/common/*.c"
    "${RENUT_ZSTD_LIB_DIR}/decompress/*.c")
add_library(renut_zstd STATIC ${renut_zstd_sources})
target_include_directories(renut_zstd PUBLIC "${RENUT_ZSTD_LIB_DIR}")
target_compile_definitions(renut_zstd PRIVATE ZSTD_DISABLE_ASM)
set_target_properties(renut_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Shader containers are keyed by XXH3_64, hashed with the same xxHash
# XenosRecomp uses.
set(RENUT_XXHASH_DIR "${RENUT_XENOS_RECOMP_DIR}/thirdparty/xxHash")
add_library(renut_xxhash STATIC "${RENUT_XXHASH_DIR}/xxhash.c")
target_include_directories(renut_xxhash PUBLIC "${RENUT_XXHASH_DIR}")
set_target_properties(renut_xxhash PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(RENUT_SMOLV_DIR "${RENUT_XENOS_RECOMP_DIR}/thirdparty/smol-v/source")
add_library(renut_smolv STATIC "${RENUT_SMOLV_DIR}/smolv.cpp")
target_include_directories(renut_smolv PUBLIC "${RENUT_SMOLV_DIR}")
set_target_properties(renut_smolv PROPERTIES POSITION_INDEPENDENT_CODE ON)

# renut_host_shader(<target> <stem> <profile>): tw2de (2026-10-23): plume's deko3d back end takes the GLSL of
# shaders/deko3d/<stem>.glsl, which the console compiles itself with uam (as the game's shaders); no SPIR-V or DXIL in
# the NRO. tw2de (2026-11-06): and its DKSH, made with uam at build time, in the NRO's romfs (below). app/cmake/tw2de_glsl_blob.cmake wraps the text in plume's
# Deko3DShaderBlob, as host_shaders/<stem>.deko3d.h (g_<stem>_deko3d).
function(renut_host_shader TARGET STEM PROFILE)
    set(glsl "${RENUT_GPU_DIR}/shaders/deko3d/${STEM}.glsl")
    if(NOT EXISTS "${glsl}")
        message(FATAL_ERROR "No ${glsl} (the Switch build's GLSL for host shader ${STEM}; shaders/deko3d/README.md)")
    endif()
    if(PROFILE MATCHES "^vs")
        set(stage 0)
    elseif(PROFILE MATCHES "^gs")
        set(stage 3)
    elseif(PROFILE MATCHES "^ps")
        set(stage 4)
    elseif(PROFILE MATCHES "^cs")
        set(stage 5)
    else()
        message(FATAL_ERROR "renut_host_shader: no deko3d stage for profile ${PROFILE}")
    endif()
    set(script "${CMAKE_SOURCE_DIR}/cmake/tw2de_glsl_blob.cmake")
    set(out "${RENUT_GPU_GEN_DIR}/host_shaders/${STEM}.deko3d.h")
    # tw2de (2026-11-06): with devkitPro's uam (below), a shader without specialization constants goes in as its
    # metadata only (META_ONLY: no text; its one variant is the DKSH in the romfs); one with constants keeps its text
    # (its other variants are compiled on the console).
    # tw2de (2026-11-06): TW2DE_UAM is the project's own uam (app/thirdparty/uam, the compiler the console runs)
    # built as a PC program by build_switch.py (build_host_uam, out/host-uam): the same code a console compile makes.
    # Never devkitPro's uam: it made wrong code for FSR 3's accumulate and prepare_inputs passes (constant buffer values
    # read as 0 in some half-warps, a different set every frame). Empty (no PC compiler): no DKSH at build time, the
    # console compiles every host shader at its first use.
    set(TW2DE_UAM "" CACHE FILEPATH "the PC build of app/thirdparty/uam (build_switch.py); empty: none")
    file(READ "${glsl}" glsl_text)
    # Shaders the console compiles at its first use even with TW2DE_UAM (their text, no DKSH, no name:
    # tw2de_glsl_blob.cmake NAMED=OFF). Empty now; the means kept for a shader a build-time compile must not make.
    set(console_uam_shaders)
    set(console_uam OFF)
    if(STEM IN_LIST console_uam_shaders)
        set(console_uam ON)
    endif()
    set(meta_only OFF)
    if(TW2DE_UAM AND EXISTS "${TW2DE_UAM}" AND NOT console_uam AND NOT glsl_text MATCHES "SPIRV_CROSS_CONSTANT_ID")
        set(meta_only ON)
    endif()
    set(named ON)
    if(console_uam)
        set(named OFF)
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${glsl}")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${glsl} -DSTEM=${STEM} -DSTAGE=${stage} -DOUT=${out} -DMETA_ONLY=${meta_only}
                -DNAMED=${named} -P "${script}"
        DEPENDS "${glsl}" "${script}"
        COMMENT "Embedding host shader ${STEM}.glsl (deko3d)"
        VERBATIM)
    target_sources(${TARGET} PRIVATE "${out}")
    # tw2de (2026-11-06): its DKSH made now with TW2DE_UAM (the project's uam, as the console compiles a host
    # shader: uam::compile with the tw2de codegen flags at 0), for the NRO's romfs as shaders/<stem>.dksh
    # (app/CMakeLists.txt stages it; the blob carries the name, StageNamed). Only the default variant; never put in the
    # SD card's DKSH cache. Without TW2DE_UAM the console compiles it at its first use.
    if(TW2DE_UAM AND EXISTS "${TW2DE_UAM}" AND NOT console_uam)
        if(stage EQUAL 0)
            set(uam_stage vert)
        elseif(stage EQUAL 3)
            set(uam_stage geom)
        elseif(stage EQUAL 4)
            set(uam_stage frag)
        else()
            set(uam_stage comp)
        endif()
        set(shader_dir "${CMAKE_BINARY_DIR}/romfs_shaders")
        set(dksh "${shader_dir}/${STEM}.dksh")
        add_custom_command(
            OUTPUT "${dksh}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
            COMMAND "${TW2DE_UAM}" -s ${uam_stage} -o "${dksh}" "${glsl}"
            DEPENDS "${glsl}" "${TW2DE_UAM}"
            COMMENT "Compiling host shader ${STEM}.glsl with uam (DKSH for the NRO's romfs)"
            VERBATIM)
        target_sources(${TARGET} PRIVATE "${dksh}")
        set_property(GLOBAL APPEND PROPERTY TW2DE_ROMFS_SHADERS "${dksh}")
        set(TW2DE_ROMFS_SHADER_DIR "${shader_dir}" CACHE INTERNAL "")
    endif()
endfunction()

# ---------------------------------------------------------------------------
function(renut_gpu_setup TARGET)
    target_sources(${TARGET} PRIVATE
        # The host device, the frame and the render thread.
        ${RENUT_GPU_DIR}/backend/backend.cpp
        ${RENUT_GPU_DIR}/backend/compile_notice.cpp  # tw2de (2026-10-22)
        ${RENUT_GPU_DIR}/backend/achievement_popup.cpp  # tw2de (2026-10-23)
        ${RENUT_GPU_DIR}/backend/popup_compose.cpp  # tw2de (2026-10-23)
        ${RENUT_GPU_DIR}/backend/gpu_profile.cpp
        ${RENUT_GPU_DIR}/backend/memory_report.cpp
        ${RENUT_GPU_DIR}/backend/guest_gpu_probe.cpp  # tw2de (2026-10-30): the GPU over guest memory, a trial
        ${RENUT_GPU_DIR}/backend/guest_gpu_memory.cpp  # tw2de (2026-10-30): the GPU's mapping of guest memory
        ${RENUT_GPU_DIR}/backend/native_window.cpp
        ${RENUT_GPU_DIR}/backend/present.cpp
        ${RENUT_GPU_DIR}/backend/queued.cpp
        ${RENUT_GPU_DIR}/backend/render_thread.cpp
        ${RENUT_GPU_DIR}/backend/video.cpp
        # The guest D3D runtime, native, and its hooks.
        ${RENUT_GPU_DIR}/d3d/d3d_native_state.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/device.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/draw.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/resource.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/shader.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/state.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/query.cpp  # TW2 (2026-11-12): occlusion queries counted by the host GPU
        # Guest-side records, command buffers and callbacks.
        ${RENUT_GPU_DIR}/guest/callbacks.cpp
        ${RENUT_GPU_DIR}/guest/command_buffers.cpp
        ${RENUT_GPU_DIR}/guest/guest_records.cpp
        ${RENUT_GPU_DIR}/guest/tw2de_guest.cpp

        # The app's side of the detached mode.
        ${RENUT_GPU_DIR}/overlay/app_integration.cpp
        # Draws, targets, textures, shaders and formats.
        ${RENUT_GPU_DIR}/render/draw.cpp
        ${RENUT_GPU_DIR}/render/fetch_filters.cpp  # tw2de (2026-10-31): the tfetches' own filters
        ${RENUT_GPU_DIR}/render/format.cpp
        ${RENUT_GPU_DIR}/render/graphics_options.cpp
        ${RENUT_GPU_DIR}/render/dynamic_resolution.cpp  # tw2de (2026-11-07, dynamic resolution)
        ${RENUT_GPU_DIR}/render/pipeline.cpp
        ${RENUT_GPU_DIR}/render/shader_library.cpp
        ${RENUT_GPU_DIR}/render/fsr1.cpp  # tw2de (2026-10-25, FSR 1's RCAS)
        ${RENUT_GPU_DIR}/render/early_upscale.cpp  # tw2de (2026-11-10): the scene upscaled before its post-processing
        ${RENUT_GPU_DIR}/render/fsr3.cpp  # tw2de (2026-11-06, FSR 3.1 with the scene's motion vectors)
        ${RENUT_GPU_DIR}/render/surfaces.cpp
        ${RENUT_GPU_DIR}/render/texture_layout.cpp
        ${RENUT_GPU_DIR}/render/textures.cpp
        ${RENUT_GPU_DIR}/render/upload_ring.cpp
        ${RENUT_GPU_DIR}/render/vertex_format.cpp)
    # TW2 (2026-11-13, diagnostic): line tables for the feed thread's sources, so the samples of a console profile
    # map to source lines (addr2line on out/switch/tw2de.elf). -g1 changes no code; the NRO has no debug sections.
    set_source_files_properties(${RENUT_GPU_DIR}/render/draw.cpp ${RENUT_GPU_DIR}/render/textures.cpp
        ${RENUT_GPU_DIR}/render/pipeline.cpp ${RENUT_GPU_DIR}/render/surfaces.cpp PROPERTIES COMPILE_OPTIONS "-g1")

    renut_host_shader(${TARGET} present_vs vs_6_0)
    renut_host_shader(${TARGET} present_ps ps_6_0)
    renut_host_shader(${TARGET} notice_ps ps_6_0)  # tw2de (2026-10-22): backend/compile_notice.cpp
    renut_host_shader(${TARGET} popup_ps ps_6_0)  # tw2de (2026-10-23): backend/achievement_popup.cpp
    renut_host_shader(${TARGET} depth_resolve_ps ps_6_0)
    renut_host_shader(${TARGET} depth_resolve_ms_ps ps_6_0)
    renut_host_shader(${TARGET} texture_mip_ps ps_6_0)
    renut_host_shader(${TARGET} resolve_copy_ps ps_6_0)  # tw2de (2026-10-18)
    renut_host_shader(${TARGET} resolve_copy_ms_ps ps_6_0)  # tw2de: one sample of a multisampled target
    # tw2de (2026-10-25, tw2de_renut_upscale_filter): Renut's upscalers.
    renut_host_shader(${TARGET} lanczos2_ps ps_6_0)
    renut_host_shader(${TARGET} depth_downsample_ps ps_6_0)  # TW2 (2026-11-12): render/draw.cpp HalfDepthFillLocked
    renut_host_shader(${TARGET} upscale_l15_ps ps_6_0)
    renut_host_shader(${TARGET} upscale_bilinear_ps ps_6_0)  # tw2de (2026-10-25): filter 4
    renut_host_shader(${TARGET} fullscreen_vs vs_6_0)  # tw2de (2026-10-25): render/fsr1.cpp's RCAS pass
    # tw2de (2026-10-25): FSR 1 (AMD FidelityFX Super Resolution 1.0, MIT): EASU as the upscale's pixel shader.
    renut_host_shader(${TARGET} fsr1_easu_ps ps_6_0)
    renut_host_shader(${TARGET} fsr1_rcas_ps ps_6_0)  # render/fsr1.cpp
    renut_host_shader(${TARGET} early_upscale_ps ps_6_0)  # tw2de (2026-11-10): render/early_upscale.cpp
    renut_host_shader(${TARGET} early_easu_ps ps_6_0)  # tw2de (2026-11-10): render/early_upscale.cpp, FSR 1
    # tw2de (2026-11-07): the scan-out upscaled to a 1920x1080 output (a 1280x720 scene docked on a 1080p TV).
    renut_host_shader(${TARGET} present_lanczos_ps ps_6_0)
    renut_host_shader(${TARGET} present_easu_ps ps_6_0)
    renut_host_shader(${TARGET} rectangle_list_gs gs_6_0)
    renut_host_shader(${TARGET} point_sprite_gs gs_6_0)
    renut_host_shader(${TARGET} stream_convert_cs cs_6_0)
    renut_host_shader(${TARGET} target_readback_cs cs_6_0)
    renut_host_shader(${TARGET} fsr3_prepare_inputs_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    renut_host_shader(${TARGET} fsr3_shading_change_pyramid_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    renut_host_shader(${TARGET} fsr3_shading_change_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    renut_host_shader(${TARGET} fsr3_prepare_reactivity_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    renut_host_shader(${TARGET} fsr3_luma_instability_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    renut_host_shader(${TARGET} fsr3_accumulate_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    renut_host_shader(${TARGET} fsr3_accumulate_li_bil_cs cs_6_0)  # tw2de (2026-11-07): render/fsr3.cpp, native resolution
    renut_host_shader(${TARGET} fsr3_rcas_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    # tw2de (2026-11-07): FSR's sharpening (RCAS, xc1fsr's adaptive one) and the accumulates that leave it the output.
    renut_host_shader(${TARGET} fsr3_rcas_adapt_cs cs_6_0)
    renut_host_shader(${TARGET} fsr3_accumulate_sharp_cs cs_6_0)
    renut_host_shader(${TARGET} fsr3_accumulate_li_bil_sharp_cs cs_6_0)
    renut_host_shader(${TARGET} fsr3_clear_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    renut_host_shader(${TARGET} fsr3_clear_uint_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    renut_host_shader(${TARGET} fsr3_clear_buffer_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp
    renut_host_shader(${TARGET} fsr3_view_cs cs_6_0)  # tw2de (2026-11-06): render/fsr3.cpp (tw2de_fsr3_diag)

    # The game's shaders: translated on the console (app/cmake/tw2de_renut_gpu.cmake).
    tw2de_renut_shader_cache(${TARGET})

    target_include_directories(${TARGET} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${RENUT_GPU_DIR}/shaders"
        "${RENUT_GPU_GEN_DIR}"
        "${RENUT_SHADER_COMMON_DIR}")
    # Ahead of the SDK's include directory, which ships a different xxhash.h.
    target_include_directories(${TARGET} BEFORE PRIVATE "${RENUT_XXHASH_DIR}")

    target_compile_definitions(${TARGET} PRIVATE RENUT_RECOMP)
    target_link_libraries(${TARGET} PRIVATE plume renut_zstd renut_smolv renut_xxhash rex::o1heap)
endfunction()

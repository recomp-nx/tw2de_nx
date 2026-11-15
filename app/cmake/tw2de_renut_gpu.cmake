# tw2de (2026-10-16): Renut's Direct3D-level renderer (github.com/masterspike52/reNut, imported into
# src/renut_engine/gpu by tools/renut_gpu_import.py), the game's renderer, on plume's deko3d back end. Its gpu.cmake (made
# Switch-only on 2026-10-29) with its dependencies where it looks for them: thirdparty/plume (plume at Renut's version),
# thirdparty/reblue-XenosRecomp (its translator, zstd, smol-v, xxHash).
#
# An OBJECT library, not a static one: its hooks replace the generated code's weak sub_X definitions, and a static
# archive's members are only pulled in for undefined symbols, so the hooks would silently not be linked.

# Renut's SDK exports o1heap as rex::o1heap; tw2de's SDK fork names it o1heap.
if(TARGET o1heap AND NOT TARGET rex::o1heap)
    add_library(rex::o1heap ALIAS o1heap)
endif()

# The Switch's plume: the deko3d back end only (renut_gpu_backend = "deko3d", docs/deko3d-renderer.md), no Vulkan
# driver, and the uam it compiles shaders with on the console.
set(PLUME_PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
set(PLUME_SWITCH_DEVKITPRO "${DEVKITPRO}" CACHE PATH "" FORCE)
set(PLUME_SWITCH_UAM_DIR "${CMAKE_SOURCE_DIR}/thirdparty/uam" CACHE PATH "" FORCE)
# The host shaders: their GLSL (src/renut_engine/gpu/shaders/deko3d), compiled on the console (gpu.cmake
# renut_host_shader).
# tw2de (2026-10-21): no game shader in the NRO. The console translates them itself (XenosRecomp's GLSL mode
# compiled in: src/renut_engine/gpu/nx_shaders) into a store on the SD card, before the game starts (the shader
# pre-pass over the game files) and when the game creates one (default.xex's), and uam compiles them.
function(tw2de_renut_shader_cache TARGET)
    set(_nxs "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/nx_shaders")
    set(_xr "${CMAKE_SOURCE_DIR}/thirdparty/reblue-XenosRecomp/XenosRecomp")
    # shader_common_glsl.h's text, which every translation has in it.
    set(_hdr_cpp "${CMAKE_BINARY_DIR}/nx_shaders/xr_glsl_header.cpp")
    set(XR_HEADER "${_xr}/shader_common_glsl.h")
    file(SIZE "${XR_HEADER}" XR_SIZE)
    # tw2de (2026-11-08): the text's hash in the generated file too, so it changes (and is compiled again) whenever
    # the header does: .incbin is not a dependency the compiler reports, and an edit of the same size left the old text
    # in the NRO (the dynamic resolution's g_DrsStep at 1352 instead of 1356).
    file(SHA1 "${XR_HEADER}" XR_SHA1)
    configure_file("${_nxs}/xr_glsl_header.cpp.in" "${_hdr_cpp}" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_xr}/shader_common_glsl.h")
    target_sources(${TARGET} PRIVATE
        "${_hdr_cpp}"
        "${_xr}/shader_recompiler.cpp"
        "${_nxs}/xenos_glsl.cpp"
        "${_nxs}/shader_scan.cpp"
        "${_nxs}/shader_store.cpp"
        "${_nxs}/container_cache.cpp"
        "${_nxs}/late_spec.cpp"
        "${_nxs}/front_end_store.cpp"
        "${_nxs}/shader_prepass.cpp"
        "${_nxs}/prepass_screen.cpp"
        "${_nxs}/shader_prepass_switch.cpp"
        "${_nxs}/boot_menu_switch.cpp"  # tw2de (2026-10-25): the boot menu
        "${_nxs}/thread_alloc.cpp"
        "${_nxs}/nx_file.cpp"
        "${_nxs}/variant_list.cpp")
    set_source_files_properties("${_hdr_cpp}" "${_xr}/shader_recompiler.cpp" "${_nxs}/xenos_glsl.cpp"
        "${_nxs}/prepass_screen.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set_source_files_properties("${_xr}/shader_recompiler.cpp" PROPERTIES
        COMPILE_OPTIONS "-include;${_nxs}/xr_pch.h;-Wno-unused-variable;-Wno-switch")
    target_include_directories(${TARGET} PRIVATE "${_xr}"
        "${CMAKE_SOURCE_DIR}/thirdparty/uam"
        "${REXSDK_DIR}/thirdparty/stb")
    target_compile_definitions(${TARGET} PRIVATE RENUT_SHADER_STORE)
    # The pre-pass's compile threads allocate from slabs of their own (nx_shaders/thread_alloc.cpp): newlib's
    # malloc, which has one lock for the process, is wrapped in the executable's link.
    target_link_options(${TARGET} INTERFACE
        "-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=pthread_mutex_lock,--wrap=__malloc_lock,--wrap=_malloc_r,--wrap=_free_r,--wrap=_realloc_r,--wrap=_calloc_r,--wrap=_memalign_r")
    target_link_libraries(${TARGET} PRIVATE "${DEVKITPRO}/portlibs/switch/lib/libz.a")
endfunction()

include("${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/gpu.cmake")

add_library(tw2de_renut_gpu OBJECT
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/tw2de_renut_select.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/tw2de_renut_tuning.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/tw2de_renut_checks.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/tw2de_renut_record.cpp"
    # tw2de (2026-10-19, latency batch 3): the display's refresh for the frame limiter (it reads the presents' fences)
    "${CMAKE_SOURCE_DIR}/src/renut_engine/display_sync.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/frame_interval.cpp"  # tw2de (2026-10-25)
    "${CMAKE_SOURCE_DIR}/src/renut_engine/frame_timing.cpp"  # tw2de (2026-11-05, tw2de_diag_vsync_timing)
    "${CMAKE_SOURCE_DIR}/src/renut_engine/perf/frame_stats.cpp")
target_include_directories(tw2de_renut_gpu PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}"
    # tw2de_renut_tuning.cpp: the Switch thread helpers (threading_switch.h, through src/me/me_platform_compat.h).
    "${REXSDK_DIR}/src/core")
target_link_libraries(tw2de_renut_gpu PRIVATE rex::runtime)
# tw2de (2026-10-17): the deko3d back end's glue (docs/deko3d-renderer.md).
target_sources(tw2de_renut_gpu PRIVATE "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/tw2de_renut_deko3d.cpp")
if(COMMAND rexglue_apply_target_settings)
    rexglue_apply_target_settings(tw2de_renut_gpu)
endif()
renut_gpu_setup(tw2de_renut_gpu)
# tw2de (2026-10-20): no Vulkan driver: Renut always boots on deko3d (tw2de_renut_select.cpp, backend.cpp
# ResolveBackend).
target_compile_definitions(tw2de_renut_gpu PRIVATE TW2DE_SWITCH_NO_VULKAN=1)
target_link_libraries(tw2de PRIVATE tw2de_renut_gpu)
target_compile_definitions(tw2de PRIVATE TW2DE_RENUT_RENDERER=1)

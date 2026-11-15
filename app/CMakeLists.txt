# tw2de: an Xbox 360 game recompiled with ReXGlue, for the Nintendo Switch (only: no PC build since 2026-10-29).
#
# The renderer: reNut's Direct3D-level renderer through plume's deko3d back end.

cmake_minimum_required(VERSION 3.25)
project(tw2de LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT REXGLUE_PLATFORM_SWITCH)
    message(FATAL_ERROR "tw2de builds for the Nintendo Switch only: configure with "
                        "tools/switch/cmake/switch-devkitA64.cmake (build_switch.py does it)")
endif()

# Switch toolchain flags (see the measurements behind them in the project's history).
add_compile_options(-mearly-ra=none -mno-outline-atomics)
add_link_options(-mearly-ra=none)

# tw2de (2026-10-30): the optimisation level and link-time optimisation (build_switch.py --opt O2|O3 --lto, each
# such build in a folder of its own). Set before the SDK is added,
# so its targets get them too. TW2DE_OPT empty: as before (-O3: CMake's Release flags and the SDK's own).
# uam keeps its own -O2 (its CMakeLists), the function registration its -O0 (below). With TW2DE_LTO, every C and C++
# object but uam's (thirdparty/plume/CMakeLists.txt: Mesa's front end came out wrong with it) holds GCC's intermediate
# code (no machine code: -flto's default, no fat objects) and the NRO's link compiles
# the whole program at once, with the same level (-flto=auto: the link uses as many jobs as there are cores), and
# -fipa-pta. With PGO: the counters are placed when each file is compiled, so a profile made at the same level without
# LTO fits (build_switch.py: pgo/ for -O3, pgo-o2/ for -O2).
set(TW2DE_OPT "" CACHE STRING "Optimisation level of every target: O2, O3, or empty (O3, as before)")
option(TW2DE_LTO "Link-time optimisation of the whole NRO" OFF)
if(TW2DE_OPT)
    if(NOT TW2DE_OPT MATCHES "^O[23]$")
        message(FATAL_ERROR "TW2DE_OPT is O2, O3 or empty, not '${TW2DE_OPT}'")
    endif()
    set(CMAKE_C_FLAGS_RELEASE "-${TW2DE_OPT} -DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-${TW2DE_OPT} -DNDEBUG")
    set(REXGLUE_RELEASE_OPT "-${TW2DE_OPT}")
endif()
set(TW2DE_OPT_FLAG "-O3")
if(TW2DE_OPT)
    set(TW2DE_OPT_FLAG "-${TW2DE_OPT}")
endif()
if(TW2DE_LTO)
    # -fipa-pta: points-to analysis across functions (and, at the LTO link, across the whole program): which memory a
    # pointer can reach, so loads and stores through others may move past it. Its cost grows with the program: the
    # link's time and memory above all.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-flto=auto> $<$<COMPILE_LANGUAGE:C,CXX>:-fipa-pta>)
    # The link compiles the code: the same level, and the options the results depend on given again (GCC keeps them
    # with each function too): no fused multiply-add (the guest's float results), no outline atomics.
    add_link_options(-flto=auto ${TW2DE_OPT_FLAG} -fipa-pta -ffp-contract=off -fno-strict-aliasing
                     -mno-outline-atomics)
endif()
message(STATUS "tw2de: optimisation ${TW2DE_OPT_FLAG}, link-time optimisation ${TW2DE_LTO}")

# tw2de (2026-10-21): profile-guided optimisation (src/pgo_switch.h). TW2DE_PGO=generate: every target (set before the SDK is added, so its targets too) compiled
# and linked with -fprofile-generate (the counts go to /switch/tw2de_nx/pgo on the SD card); TW2DE_PGO=use: with
# -fprofile-use from TW2DE_PGO_DIR (the folder copied back from the console), in the same build folder (GCC names a
# profile after the object's path).
set(TW2DE_PGO "" CACHE STRING "Profile-guided optimisation: generate, use or empty")
set(TW2DE_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "The profile (TW2DE_PGO=use)")
# tw2de (2026-10-25): the prefix as GCC sees the object's path. GCC compares it byte for byte with its working
# folder (getpwd: backslashes on Windows) joined to the object's name, so a prefix with forward slashes never matched
# there ("does not start with profile prefix"): every profile kept the whole path, out#switch-pgo#... in the profiling
# build, which the release in out/switch then looked for as out#switch#... and never found.
if(CMAKE_HOST_WIN32)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" TW2DE_PGO_PREFIX)
else()
    set(TW2DE_PGO_PREFIX "${CMAKE_BINARY_DIR}")
endif()
# tw2de (2026-10-26): the profiling build numbers its functions inside their file (the same wherever the project
# is) and writes each object's function list (-ftest-coverage: a .gcno beside it, the code unchanged); the release
# keeps GCC's own ids (a hash of the name, and for a function not visible outside its file also of its source's path),
# and tools/pgo_unpack.py gives every function of the profile the id the release looks for (tools/pgo_functions.py).
# The release cannot number them too: GCC crashes at -fprofile-use on a file with a C++ thunk when it does
# (read_thunk_profile; threading_posix.cpp, "internal compiler error: Segmentation fault").
set(TW2DE_PGO_ID --param=profile-func-internal-id=1 -ftest-coverage)
if(TW2DE_PGO STREQUAL "generate")
    # 2026-10-22: no value profiling (-fno-profile-values): its indirect-call profiler in devkitA64's prebuilt libgcov
    # reads its thread-local through TPIDR_EL0, which Horizon does not set for homebrew (libnx's TLS is behind
    # TPIDRRO_EL0, -mtp=soft): the first indirect call faulted and the instrumented build hung before its log
    # tw2de (2026-10-24): -fprofile-prefix-path: each profile named after its object's path inside the build folder
    # (a#b#c.gcda), not the whole path of this PC: the profiling build has a folder of its own (build_switch.py --pgo
    # generate: out/switch-pgo) and the release finds the counts by the same names in out/switch.
    add_compile_options(-fprofile-generate=/switch/tw2de_nx/pgo -fprofile-update=prefer-atomic -fno-profile-values
                        -fprofile-prefix-path=${TW2DE_PGO_PREFIX} ${TW2DE_PGO_ID})
    # tw2de (2026-10-25): every object's profile registered with src/pgo_switch.cpp instead of libgcov (one file per
    # run written there; libgcov's one file per object never finished on the SD card).
    add_link_options(-fprofile-generate=/switch/tw2de_nx/pgo -fno-profile-values -Wl,--wrap=__gcov_init)
    add_compile_definitions(TW2DE_PGO_GENERATE=1)
    # tw2de (2026-10-30): where src/pgo_switch.cpp writes its runs (the -O2 profiling build: /switch/tw2de_nx/pgo-o2,
    # build_switch.py). The counters' own names keep /switch/tw2de_nx/pgo (tools/pgo_unpack.py reads them so).
    set(TW2DE_PGO_RUN_DIR "/switch/tw2de_nx/pgo" CACHE STRING "The SD card's folder for the profiling build's runs")
    add_compile_definitions(TW2DE_PGO_RUN_DIR="${TW2DE_PGO_RUN_DIR}")
elseif(TW2DE_PGO STREQUAL "use")
    # 2026-10-25: -Wno-missing-profile dropped: an object without its counts is now reported (a path mismatch showed
    # nothing before).
    # tw2de (2026-10-26): -fprofile-correction: counts that do not add up (a branch's edges against its block's
    # count) are smoothed instead of stopping the build ("corrupted profile info: profile data is not flow-consistent",
    # glcpp-parse.y): the pre-pass part scaled down by tools/pgo_unpack.py rounds each count on its own, its snapshot
    # was taken while threads ran, and the counters of code several threads run are not updated as one.
    add_compile_options(-fprofile-use=${TW2DE_PGO_DIR} -fprofile-partial-training -fprofile-correction
                        -fno-profile-values -Wno-coverage-mismatch -fprofile-prefix-path=${TW2DE_PGO_PREFIX})
    add_link_options(-fprofile-use=${TW2DE_PGO_DIR} -fprofile-partial-training -fprofile-correction
                     -fno-profile-values)
    # tw2de (2026-10-26): the profile's id (build_switch.py: from pgo/'s runs) in every compile line: a new
    # profile compiles everything again with it, the same one nothing (the .gcda files are not in ninja's dependencies).
    if(TW2DE_PGO_PROFILE_ID)
        add_compile_definitions(TW2DE_PGO_PROFILE_ID=${TW2DE_PGO_PROFILE_ID})
    endif()
endif()

include(generated/rexglue.cmake)
# tw2de (2026-10-26): the function registration (one function of about 44 000 calls, run once at start) without
# optimisation: GCC's -O2 on it took 81 minutes on one core, the build's longest step by far. Appended to the generated
# sources' options, so it comes after -O2 and wins. Without the precompiled header (made with -O2, GCC refuses it for
# -O0: "not used because '__OPTIMIZE__' not defined"); the file includes the same headers itself (tw2de_init.h).
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/tw2de_register.cpp" APPEND PROPERTY COMPILE_OPTIONS -O0)
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/tw2de_register.cpp" PROPERTY SKIP_PRECOMPILE_HEADERS ON)

set(TW2DE_SOURCES
    src/main.cpp
    src/cpu/tw2de_enhancements.cpp
    src/tw2de_renderer_shared.cpp
    src/tw2de_stall_dump.cpp
    # src/cpu/tw2de_wdb_diag.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/tw2de_spin_barrier.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/tw2de_slot_wait_diag.cpp  [tw2de (2026-11-09): FF XIII hooks, out until ported]
    # src/tw2de_frame_pacing.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/cpu/tw2de_native_zlib.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    src/cpu/tw2de_native_crt.cpp
    src/cpu/tw2de_usm_ffmpeg.cpp  # TW2 (2026-11-12): the USM movies' MPEG video by FFmpeg (tw2de_usm_ffmpeg)
    # src/cpu/tw2de_native_vmath.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/cpu/tw2de_native_audio.cpp  [tw2de (2026-11-09): FF XIII hooks, out until ported]
    # src/cpu/tw2de_sync_diag.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/cpu/tw2de_job_diag.cpp  # tw2de (2026-10-25): tw2de_diag_jobs  [tw2de (2026-11-09): FF XIII hooks, out until ported]
    # src/cpu/tw2de_native_locks.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/cpu/tw2de_native_vector.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/cpu/tw2de_mover_diag.cpp  # tw2de (2026-11-06): tw2de_diag_block_mover  [tw2de (2026-11-09): FF XIII hooks, out until ported]
    src/cpu/tw2de_parallel_memmove.cpp  # tw2de (2026-11-06): the game's large memmoves on 3 cores
    src/tw2de_romfs.cpp  # tw2de (2026-11-06): the port's own files in the NRO's romfs
    # src/cpu/tw2de_render_resolution.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    src/cpu/tw2de_texture_sources.cpp  # tw2de (2026-10-25): tw2de_diag_texture_sources
    # src/cpu/tw2de_skip_boot_prompts.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/cpu/tw2de_disc_swap.cpp  # tw2de (2026-10-31): the disc swaps, every disc in the game folder  [tw2de (2026-11-09): FF XIII-2 has one disc: removed]
    # src/cpu/tw2de_game_log.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/cpu/tw2de_dflags_diag.cpp  # tw2de (2026-11-01): tw2de_diag_dflags, the game's debug switches  [tw2de (2026-11-09): FF XIII hooks, out until ported]
    # src/cpu/tw2de_load_fade_fix.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/cpu/tw2de_touch_diag.cpp  # tw2de (2026-11-01): tw2de_diag_touch_off, field collision bodies kept off  [tw2de (2026-11-09): FF XIII hooks, out until ported]
    # src/cpu/tw2de_ai_axis_diag.cpp  # tw2de (2026-11-01): tw2de_diag_ai_axis, the field AI's angle function  [tw2de (2026-11-09): FF XIII hooks, out until ported]
    # src/cpu/tw2de_mesh_proxy_diag.cpp  # tw2de (2026-11-01): tw2de_diag_mesh_proxy, the physics reading model vertices  [tw2de (2026-11-09): FF XIII hooks, out until ported]
    # src/cpu/tw2de_move_floor_diag.cpp  # tw2de (2026-11-01): tw2de_diag_move_floor, the boat's clips and bodies  [tw2de (2026-11-09): FF XIII hooks, out until ported]
    # src/cpu/tw2de_switch_texts.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    # src/cpu/tw2de_menu_trim.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    src/cpu/tw2de_lang_pack.cpp  # TW2 (2026-11-12): kept (no hooks; inactive without a language pack / a matching texture)
    src/cpu/tw2de_glyphs.cpp  # TW2 (2026-11-12): kept (no hooks; inactive without a language pack / a matching texture)
    # src/cpu/tw2de_texts.cpp  [TW2 (2026-11-12): FF hooks, out until ported to TW2]
    src/cpu/tw2de_spin_waits.cpp  # TW2 (2026-11-12): the job system's busy wait yields the core
    src/cpu/tw2de_language.cpp  # TW2 (2026-11-12): the boot menu's language as the game's at start-up
    src/cpu/tw2de_switch_ui.cpp  # TW2 (2026-11-14): the game's interface for the Switch (pack0.dzip's patched view)
    src/cpu/tw2de_switch_ui_core.cpp
    src/cpu/tw2de_render_frame.cpp  # TW2 (2026-11-12): the render thread's refresh frames (the black frames)
    src/cpu/tw2de_native_reverb.cpp  # TW2 (2026-11-14): FMOD's reverb (sub_82968720) as native code
    src/cpu/tw2de_scene_size.cpp  # TW2 (2026-11-14): the game's 3D scene at another size (sub_82EF0328)
    src/cpu/tw2de_camera_speed.cpp  # TW2 (2026-11-16): the camera's input from the right stick scaled (sub_82E7A208)
    src/cpu/tw2de_job_check.cpp  # TW2 (2026-11-12): diagnostic of the job worker crash (tw2de_diag_jobs)
    src/cpu/tw2de_tw2_stubs.cpp  # TW2 (2026-11-12): the names the renderer uses from the files left out
    src/cpu/tw2de_images.cpp  # TW2 (2026-11-12): kept (no hooks; inactive without a language pack / a matching texture)
    src/cpu/tw2de_layouts.cpp  # TW2 (2026-11-12): kept (no hooks; inactive without a language pack / a matching texture)
    src/cpu/tw2de_wpd_patch.cpp  # TW2 (2026-11-12): kept (no hooks; inactive without a language pack / a matching texture)
    src/cpu/tw2de_switch_buttons.cpp  # TW2 (2026-11-12): kept (no hooks; inactive without a language pack / a matching texture)
    src/cpu/tw2de_handheld_gpu.cpp  # tw2de (2026-10-25): tw2de_handheld_gpu_460
    # tw2de (2026-10-24): the CPU profiler (tw2de_cpu_profiler: the sampler, slow-frame reports, stall watchdog)
    src/profiler/profiler.cpp
    src/profiler/cpu_profiler_switch.cpp
    src/profiler/stall_watch_switch.cpp
)

if(TW2DE_PGO STREQUAL "generate")
    list(APPEND TW2DE_SOURCES src/pgo_switch.cpp)
endif()

add_executable(tw2de ${TW2DE_SOURCES})

# tw2de (2026-11-09): FF XIII's budgeted heap compaction walk (made here from its sub_8279A0B8, with
# cpu/tw2de_mover_diag.cpp) is out until that function is found in FF XIII-2.
rexglue_setup_target(tw2de GPU_PLUGINS xenos)
# tw2de (2026-10-24): the linked Switch executable as out/switch/tw2de_nx.elf (it had no extension); the NRO is made
# from it (rexglue_switch.cmake) and keeps its name, tw2de.nro.
set_target_properties(tw2de PROPERTIES SUFFIX ".elf")

if(TARGET rexcore)
    target_compile_definitions(rexcore PRIVATE REX_SWITCH_FALLBACK_EXE="/switch/tw2de_nx/tw2de_nx.nro")
endif()

# The renderer: reNut's Direct3D-level renderer on plume's deko3d back end.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/tw2de_renut_gpu.cmake")

# The guest's float results must not change (no fused multiply-add it did not have), and the translated code reads
# the same memory as different types.
foreach(_t tw2de tw2de_recomp)
    if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE -ffp-contract=off -fno-strict-aliasing)
    endif()
endforeach()
# tw2de (2026-11-01, diagnostic): build_switch.py --portable-fp: the instruction helpers of rex/ppc in their portable
# form (the expressions the code generator emitted before the AArch64 ones: as on x86, close to Xenia) instead of the
# AArch64 forms (vmx_fp.h's NEON paths, fpu_est.h's hardware tables, intrinsics.h's NEON helpers, context.h's record
# form). To tell whether one of those changes what the game does.
option(TW2DE_PORTABLE_FP "Diagnostic: the portable forms of the PowerPC instruction helpers" OFF)
if(TW2DE_PORTABLE_FP)
    foreach(_t tw2de tw2de_recomp)
        if(TARGET ${_t})
            target_compile_definitions(${_t} PRIVATE REX_VMX_FP_PORTABLE=1 REX_VMX_HELPERS_X86_FORM=1
                                                     REX_SETFROMMASK_MOVEMASK=1)
        endif()
    endforeach()
    message(STATUS "tw2de: TW2DE_PORTABLE_FP: the portable instruction helpers (diagnostic)")
endif()

target_include_directories(tw2de PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${REXSDK_DIR}/src/core"
    "${REXSDK_DIR}/thirdparty/stb")  # tw2de (2026-10-25): stb_image for the language pack's glyphs
# tw2de (2026-10-23): native zlib for the game's decompression (src/cpu/tw2de_native_zlib.cpp).
target_link_libraries(tw2de PRIVATE "${DEVKITPRO}/portlibs/switch/lib/libz.a")
# TW2 (2026-11-12): the SDK's FFmpeg (src/cpu/tw2de_usm_ffmpeg.cpp: MPEG-1/2 video, ADX)
if(TARGET libavcodec)
    target_link_libraries(tw2de PRIVATE libavcodec libavutil)
endif()
target_include_directories(tw2de PRIVATE "${DEVKITPRO}/portlibs/switch/include")
foreach(_dep xxhash)
    if(TARGET ${_dep})
        target_include_directories(tw2de PRIVATE $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()

# tw2de (2026-10-27): the NRO's icon, app/icon/icon.jpg (256x256 JPEG) when there is one (libnx's default
# otherwise). The
# folder is a configure dependency, so an icon put there later is picked up (a changed one relinks: LINK_DEPENDS).
# tw2de (2026-11-08): app/icon/icon.png, when there is one, comes first: at build time scaled to 256x256 and saved as
# a JPEG of at most 100 kB (102,400 bytes) (tools/make_icon.py: the highest quality that fits; the Python standard library only)
# into the build folder; a changed PNG makes it again.
set(TW2DE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.jpg")
set(TW2DE_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png")
set(TW2DE_ICON_ARG)
if(EXISTS "${TW2DE_ICON_PNG}")
    find_program(TW2DE_PYTHON NAMES python3 python py NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(TW2DE_ICON_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../tools/make_icon.py")
    set(TW2DE_ICON "${CMAKE_CURRENT_BINARY_DIR}/icon/icon.jpg")
    add_custom_command(OUTPUT "${TW2DE_ICON}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/icon"
        COMMAND "${TW2DE_PYTHON}" "${TW2DE_ICON_SCRIPT}" "${TW2DE_ICON_PNG}" "${TW2DE_ICON}" 256 102400
        DEPENDS "${TW2DE_ICON_PNG}" "${TW2DE_ICON_SCRIPT}"
        COMMENT "Making the NRO's icon from app/icon/icon.png"
        VERBATIM)
    add_custom_target(tw2de_icon DEPENDS "${TW2DE_ICON}")
    add_dependencies(tw2de tw2de_icon)
    set(TW2DE_ICON_ARG ICON "${TW2DE_ICON}")
elseif(EXISTS "${TW2DE_ICON}")
    set(TW2DE_ICON_ARG ICON "${TW2DE_ICON}")
endif()
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/icon")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icon")
endif()
# tw2de (2026-11-06): the port's own files in the NRO's romfs (src/tw2de_romfs.h): the repository's tex folder
# as romfs:/tex, and Renut's own shaders' DKSH (made with uam at build time: gpu.cmake renut_host_shader) as
# romfs:/shaders. Copied into a staging folder before each packaging; a changed or new file in tex relinks
# (LINK_DEPENDS, the folder a configure dependency), a changed shader is compiled again (a dependency of the target).
# Not game data: that never goes in the NRO (the game's shaders are translated and compiled on the console).
set(TW2DE_TEX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tex")
set(TW2DE_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
get_property(TW2DE_ROMFS_SHADER_FILES GLOBAL PROPERTY TW2DE_ROMFS_SHADERS)
set(TW2DE_ROMFS_ARG)
if(IS_DIRECTORY "${TW2DE_TEX_DIR}" OR TW2DE_ROMFS_SHADER_FILES)
    set(TW2DE_ROMFS_STAGE
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${TW2DE_ROMFS_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${TW2DE_ROMFS_DIR}")
    if(IS_DIRECTORY "${TW2DE_TEX_DIR}")
        file(GLOB TW2DE_TEX_FILES CONFIGURE_DEPENDS "${TW2DE_TEX_DIR}/*")
        set_property(TARGET tw2de APPEND PROPERTY LINK_DEPENDS ${TW2DE_TEX_FILES})
        list(APPEND TW2DE_ROMFS_STAGE
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${TW2DE_TEX_DIR}" "${TW2DE_ROMFS_DIR}/tex")
    endif()
    if(TW2DE_ROMFS_SHADER_FILES)
        list(APPEND TW2DE_ROMFS_STAGE
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${TW2DE_ROMFS_DIR}/shaders"
            COMMAND "${CMAKE_COMMAND}" -E copy ${TW2DE_ROMFS_SHADER_FILES} "${TW2DE_ROMFS_DIR}/shaders")
    else()
        message(STATUS "tw2de: no TW2DE_UAM (the PC uam, build_switch.py): Renut's shaders are compiled on the console")
    endif()
    add_custom_command(TARGET tw2de POST_BUILD ${TW2DE_ROMFS_STAGE}
        COMMENT "Staging the NRO's romfs (tex, shaders)"
        VERBATIM)
    set(TW2DE_ROMFS_ARG ROMFS "${TW2DE_ROMFS_DIR}")
endif()
rexglue_switch_add_nro(tw2de
    NAME "TW2DE NX"
    AUTHOR "recomp-nx"
    VERSION "0.1.2"
    ${TW2DE_ICON_ARG}
    ${TW2DE_ROMFS_ARG})

# tw2de (2026-10-26): TW2DE_PGO=use: no "profile count data file not found" for code the profiling build never
# had in the game (the linker's --gc-sections dropped it: no profile written), the warning kept for everything else (a
# profile name that does not match shows there). zstd's and xxHash's copies of the shader recompiler, tiny-AES, plume's
# frame generation stub.
if(TW2DE_PGO STREQUAL "use")
    foreach(_unused_target renut_zstd renut_xxhash tiny-aes)
        if(TARGET ${_unused_target})
            target_compile_options(${_unused_target} PRIVATE -Wno-missing-profile)
        endif()
    endforeach()
    if(TARGET plume)
        set_property(SOURCE "${CMAKE_SOURCE_DIR}/thirdparty/plume/plume_lsfg_stub.cpp"
                     DIRECTORY "${CMAKE_SOURCE_DIR}/thirdparty/plume" APPEND PROPERTY COMPILE_OPTIONS -Wno-missing-profile)
    endif()
endif()

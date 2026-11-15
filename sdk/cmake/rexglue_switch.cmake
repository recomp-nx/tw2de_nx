#==========================================================
# rexglue_switch.cmake - Nintendo Switch (devkitA64 + libnx)
#
#   rexglue_switch_configure_target(<target> [GPU_PLUGINS ...])
#       what rexglue_configure_target does on Switch
#   rexglue_switch_add_nro(<target> [NAME ..] [AUTHOR ..] [VERSION ..] [ICON ..])
#       packages <target>.nro next to the executable
#
# Requires the switch-devkitA64.cmake toolchain (DEVKITPRO, compiler, specs).
#==========================================================
include_guard(GLOBAL)

# tw2de (2026-10-29): deko3d only: no Vulkan driver (NVK) is linked. switch_vulkan_stubs.c answers every Vulkan
# entry point the code references with "not available" (VK_ERROR_INITIALIZATION_FAILED, NULL). The libraries the
# title links besides the runtime:
if(NOT TARGET rex::switch_libs)
    add_library(rexglue_switch_libs INTERFACE)
    add_library(rex::switch_libs ALIAS rexglue_switch_libs)
    target_link_directories(rexglue_switch_libs INTERFACE
        "${DEVKITPRO}/portlibs/switch/lib"
        "${DEVKITPRO}/libnx/lib")
    target_link_libraries(rexglue_switch_libs INTERFACE zstd z nx stdc++ m)
    target_link_options(rexglue_switch_libs INTERFACE -pthread)
endif()

# Startup objects every title needs, built here, in the SDK's scope, where C is
# enabled: a title project may declare CXX only, and CMake cannot compile a C
# source added to a target of that directory. Linked as an OBJECT library, the
# objects reach the executable directly, never through an archive:
#   runtime_switch.cpp  libnx startup constants (applet type, heap, exception
#                       stack); they override weak libnx symbols
#   switch_elf_stubs.c  libelf stubs (tw2de: kept from the NVK build; harmless)
#   switch_vulkan_stubs.c  the Vulkan entry points, "not available"
#   switch_crash_hooks.c  abort()/exit() reports and stderr to the SD; abort
#                       must be defined before libc.a is searched
if(NOT TARGET rexglue_switch_startup)
    add_library(rexglue_switch_startup OBJECT
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/runtime_switch.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_elf_stubs.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_crash_hooks.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_perf.cpp"
        # FPS and resolution for the console overlays (SaltyNX) and Reverse-NX reading.
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_saltynx.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_apm.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_vulkan_stubs.c")
    # switch_perf.cpp registers every thread (the profiler needs their handles)
    # by wrapping libnx's threadCreate/threadClose, which pthread goes through.
    # It also counts the libnx calls that cost IPC or walk memory (fences,
    # submissions, NvMap, GPU addresses and mappings, armDCacheClean, the window
    # queue and svcSleepThread) and the time the calling threads spend inside.
    target_link_options(rexglue_switch_startup INTERFACE
        "LINKER:--wrap=threadCreate,--wrap=threadClose"
        "LINKER:--wrap=nvFenceWait,--wrap=nvGpuChannelKickoff,--wrap=nvMapCreate"
        "LINKER:--wrap=nvAddressSpaceAllocFixed,--wrap=nvioctlNvhostAsGpu_MapBufferEx"
        "LINKER:--wrap=armDCacheClean,--wrap=nwindowQueueBuffer,--wrap=bqDequeueBuffer,--wrap=nwindowDequeueBuffer"
        "LINKER:--wrap=svcSleepThread")
endif()

function(rexglue_switch_configure_target target_name)
    cmake_parse_arguments(ARG "" "" "GPU_PLUGINS" ${ARGN})

    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/windowed_app_main_switch.cpp
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)
    target_link_libraries(${target_name} PRIVATE rexglue_switch_startup)

    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")
    # The SDK headers the title includes use C++23 (std::byteswap in rex/types.h).
    target_compile_features(${target_name} PRIVATE cxx_std_23)

    # Every object of the runtime and of the GPU plugin goes into the title, as
    # the object files of the SDL build would. From an archive the linker only
    # pulls members that resolve an undefined symbol, and several do nothing
    # but override a weak libnx symbol (__libnx_exception_handler, which the
    # guest memory needs), register themselves from a static constructor
    # (cvars, kernel exports) or serve the plugin entry point.
    set(_whole "$<TARGET_FILE:rexruntime>")
    set(_libs rexruntime)
    foreach(_plugin IN LISTS ARG_GPU_PLUGINS)
        if(NOT TARGET rexgpu-${_plugin})
            message(FATAL_ERROR
                "rexglue_switch_configure_target: unknown GPU plugin '${_plugin}' "
                "(no target rexgpu-${_plugin})")
        endif()
        list(APPEND _whole "$<TARGET_FILE:rexgpu-${_plugin}>")
        list(APPEND _libs rexgpu-${_plugin})
    endforeach()
    string(JOIN "," _whole_csv ${_whole})
    target_link_options(${target_name} PRIVATE
        "LINKER:--whole-archive,${_whole_csv},--no-whole-archive")
    set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS ${_whole})
    # Still linked normally, for the build order and their own dependencies
    # (FFmpeg, ...), which stay regular archives.
    target_link_libraries(${target_name} PRIVATE ${_libs} rex::switch_libs)
endfunction()

function(rexglue_switch_add_nro target_name)
    # tw2de (2026-11-06): ROMFS: a folder packed into the NRO as its romfs (romfsInit, "romfs:/...").
    cmake_parse_arguments(ARG "" "NAME;AUTHOR;VERSION;ICON;ROMFS" "" ${ARGN})
    if(NOT ARG_NAME)
        set(ARG_NAME "${target_name}")
    endif()
    if(NOT ARG_AUTHOR)
        set(ARG_AUTHOR "ReXGlue")
    endif()
    if(NOT ARG_VERSION)
        set(ARG_VERSION "${PROJECT_VERSION}")
        if(NOT ARG_VERSION)
            set(ARG_VERSION "1.0.0")
        endif()
    endif()
    if(NOT ARG_ICON)
        set(ARG_ICON "${DEVKITPRO}/libnx/default_icon.jpg")
    endif()

    find_program(REXGLUE_NACPTOOL nacptool HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)
    find_program(REXGLUE_ELF2NRO elf2nro HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)

    # Changing the icon must re-run packaging even when executable sources are unchanged.
    set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS "${ARG_ICON}")
    set(_romfs_arg)
    if(ARG_ROMFS)
        set(_romfs_arg "--romfsdir=${ARG_ROMFS}")
    endif()
    set(_dir "$<TARGET_FILE_DIR:${target_name}>")
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${REXGLUE_NACPTOOL}" --create "${ARG_NAME}" "${ARG_AUTHOR}" "${ARG_VERSION}"
            "${_dir}/${target_name}.nacp"
        # tw2de (2026-10-23): straight from the linked ELF. elf2nro copies only the loadable segments, so a stripped
        # copy gave the same NRO byte for byte (checked on v150) and only cost a 70 MB copy; the ELF keeps its symbols.
        COMMAND "${REXGLUE_ELF2NRO}" "$<TARGET_FILE:${target_name}>"
            "${_dir}/${target_name}.nro" "--nacp=${_dir}/${target_name}.nacp" "--icon=${ARG_ICON}" ${_romfs_arg}
        COMMENT "Packaging ${target_name}.nro"
        VERBATIM)
endfunction()

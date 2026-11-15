# tw2de (2026-10-23): REXGLUE_CODEGEN_ONLY: only the code generator, rexglue (a PC program), with the part of the
# runtime it uses to load a XEX, and nothing else: no SDL3, FFmpeg, Vulkan, audio, graphics or input. For a project
# that builds its game for another machine (the Switch) and needs this tool only to generate the recompiled code.
#
# The code generator boots a minimal runtime (rex::Runtime, KernelState, the kernel's modules) to load and decrypt
# the XEX. Its runtime here is a STATIC library of exactly what that needs: rexcore and rexfilesystem whole (their
# own CMake files pick the platform's files), and the 49 sources of system/, kernel/ and ui/ below. The list is
# the closure of the symbols rexglue and rexcodegen leave undefined, over every runtime object of a full host build
# (2026-10-23); none of these files calls SDL3 or FFmpeg or includes their headers. If the code generator starts using
# more of the runtime, the link says which symbol is missing: add the file that defines it here.

set(REXGLUE_CODEGEN_RUNTIME_SOURCES
    ${REXGLUE_ROOT}/src/kernel/kernel_init.cpp
    ${REXGLUE_ROOT}/src/kernel/xam/apps/xam_app.cpp
    ${REXGLUE_ROOT}/src/kernel/xam/apps/xgi_app.cpp
    ${REXGLUE_ROOT}/src/kernel/xam/apps/xlivebase_app.cpp
    ${REXGLUE_ROOT}/src/kernel/xam/apps/xmp_app.cpp
    ${REXGLUE_ROOT}/src/kernel/xam/xam_module.cpp
    ${REXGLUE_ROOT}/src/kernel/xam/xam_user.cpp
    ${REXGLUE_ROOT}/src/kernel/xbdm/xbdm_module.cpp
    ${REXGLUE_ROOT}/src/kernel/xboxkrnl/xboxkrnl_module.cpp
    ${REXGLUE_ROOT}/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp
    ${REXGLUE_ROOT}/src/kernel/xboxkrnl/xboxkrnl_threading.cpp
    ${REXGLUE_ROOT}/src/kernel/xboxkrnl/xboxkrnl_video.cpp
    ${REXGLUE_ROOT}/src/system/achievement_manager.cpp
    ${REXGLUE_ROOT}/src/system/elf_module.cpp
    ${REXGLUE_ROOT}/src/system/embedded_metadata.cpp
    ${REXGLUE_ROOT}/src/system/export_resolver.cpp
    ${REXGLUE_ROOT}/src/system/function_dispatcher.cpp
    ${REXGLUE_ROOT}/src/system/guest_path.cpp
    ${REXGLUE_ROOT}/src/system/kernel_module.cpp
    ${REXGLUE_ROOT}/src/system/kernel_state.cpp
    ${REXGLUE_ROOT}/src/system/lzx.cpp
    ${REXGLUE_ROOT}/src/system/map_parser.cpp
    ${REXGLUE_ROOT}/src/system/mmio_handler.cpp
    ${REXGLUE_ROOT}/src/system/module.cpp
    ${REXGLUE_ROOT}/src/system/runtime.cpp
    ${REXGLUE_ROOT}/src/system/thread_state.cpp
    ${REXGLUE_ROOT}/src/system/user_module.cpp
    ${REXGLUE_ROOT}/src/system/util/native_list.cpp
    ${REXGLUE_ROOT}/src/system/util/object_table.cpp
    ${REXGLUE_ROOT}/src/system/util/xdbf_utils.cpp
    ${REXGLUE_ROOT}/src/system/xam/app_manager.cpp
    ${REXGLUE_ROOT}/src/system/xam/content_manager.cpp
    ${REXGLUE_ROOT}/src/system/xam/user_profile.cpp
    ${REXGLUE_ROOT}/src/system/xenumerator.cpp
    ${REXGLUE_ROOT}/src/system/xevent.cpp
    ${REXGLUE_ROOT}/src/system/xex_module.cpp
    ${REXGLUE_ROOT}/src/system/xfile.cpp
    ${REXGLUE_ROOT}/src/system/xiocompletion.cpp
    ${REXGLUE_ROOT}/src/system/xmemory.cpp
    ${REXGLUE_ROOT}/src/system/xmodule.cpp
    ${REXGLUE_ROOT}/src/system/xmutant.cpp
    ${REXGLUE_ROOT}/src/system/xnotifylistener.cpp
    ${REXGLUE_ROOT}/src/system/xobject.cpp
    ${REXGLUE_ROOT}/src/system/xsemaphore.cpp
    ${REXGLUE_ROOT}/src/system/xsymboliclink.cpp
    ${REXGLUE_ROOT}/src/system/xthread.cpp
    ${REXGLUE_ROOT}/src/system/xtimer.cpp
    ${REXGLUE_ROOT}/src/ui/presenter.cpp
    ${REXGLUE_ROOT}/src/ui/window.cpp
)

add_subdirectory(src/core)
add_subdirectory(src/filesystem)

add_library(rexruntime STATIC ${REXGLUE_CODEGEN_RUNTIME_SOURCES})
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system ALIAS rexruntime)
add_library(rex::kernel ALIAS rexruntime)
target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
    ${REXGLUE_ROOT}/src/filesystem/devices
)
target_link_libraries(rexruntime
    PUBLIC
        rexcore
        rexfilesystem
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
    PRIVATE
        aes128
        mspack
        o1heap
        xxHash::xxhash
)
if(WIN32)
    # dxgi: ui/presenter.cpp asks DXGI for the monitor (system libraries, in every Windows SDK).
    target_link_libraries(rexruntime PRIVATE ws2_32 PUBLIC dxgi dxguid)
    target_compile_definitions(rexruntime PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_subdirectory(src/codegen)
add_subdirectory(src/rexglue)

set(REXSYSTEM_SOURCES
    runtime.cpp
    embedded_metadata.cpp
    achievement_manager.cpp
    achievements.cpp
    kernel_state.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    xevent.cpp
    xfile.cpp
    xmutant.cpp
    xsemaphore.cpp
    xtimer.cpp
    xiocompletion.cpp
    xnotifylistener.cpp
    xenumerator.cpp
    xsocket.cpp
    xsymboliclink.cpp
    xobject.cpp
    xmodule.cpp
    guest_path.cpp

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    gpu_plugin_loader.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)

# A NRO is a single static executable: no shared libraries, no dlopen.
# rexglue_switch_configure_target links it whole into the title.
add_library(rexruntime STATIC ${REXSYSTEM_SOURCES})
# tw2de (2026-10-29): no Vulkan driver on the console (deko3d only): the Xenos GPU emulation is left out
# (gpu_plugin_loader.cpp).
set_source_files_properties(gpu_plugin_loader.cpp PROPERTIES COMPILE_DEFINITIONS REX_SWITCH_NO_GPU_PLUGIN=1)
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system ALIAS rexruntime)
add_library(rex::kernel ALIAS rexruntime)


rex_target_pch(rexruntime
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread.h>
    <rex/thread/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
)

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
    PRIVATE
        aes128
        mspack
        xxHash::xxhash
)

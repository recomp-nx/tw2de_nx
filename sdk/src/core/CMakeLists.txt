# rexcore - Core utilities library
# Foundation types, data structures, and threading primitives

add_library(rexcore OBJECT
    arena.cpp
    ppc_estimates.cpp  # tw2de (2026-11-01): vrefp, vlogefp, vexptefp, fres as the 360 computes them
    ppc_exact_log.cpp  # tw2de (2026-11-01): exact_fp_log (diagnostic codegen option) reports
    bit_map.cpp
    bit_stream.cpp
    byte_stream.cpp
    clock.cpp
    cvar.cpp
    exception_handler.cpp
    filesystem.cpp
    filesystem_wildcard.cpp
    frame_latency.cpp
    hash.cpp
    logging.cpp
    memory.cpp
    mutex.cpp
    ring_buffer.cpp
    string_buffer.cpp
    threading.cpp
    timer_queue.cpp
    utf8.cpp
    vec128.cpp
)
add_library(rex::core ALIAS rexcore)

# Platform-specific sources. tw2de (2026-10-29): the Switch (the game) and the PC that runs the code generator
# (REXGLUE_CODEGEN_ONLY: Windows, or Linux); no PC build of the game.
if(WIN32)
    target_sources(rexcore PRIVATE
        atomic_win.cpp
        clock_win.cpp
        dbg_win.cpp
        dynlib_win.cpp
        exception_handler_win.cpp
        filesystem_win.cpp
        fiber_win32.cpp
        mapped_memory_win.cpp
        math_msvc.cpp
        memory_win.cpp
        platform/console_win.cpp
        platform/env_win.cpp
        seh_win.cpp
        socket_win.cpp
        system_win.cpp
        threading_win.cpp
    )
    target_compile_definitions(rexcore PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(REXGLUE_PLATFORM_SWITCH)
    # Nintendo Switch (Horizon, libnx, devkitA64). CMAKE_SYSTEM_NAME is Generic,
    # so neither WIN32 nor UNIX is defined and a branch of its own is needed.
    # Most of the POSIX layer compiles as is on newlib; what Horizon lacks goes
    # into the *_switch.cpp files. The reasons for each are in
    # docs/platform-notes.md.
    target_sources(rexcore PRIVATE
        atomic_posix.cpp
        clock_posix.cpp
        dbg_posix.cpp
        filesystem_posix.cpp          # patched: 64-bit off_t without *64 variants
        math_gcc.cpp
        platform/console_posix.cpp
        platform/env_posix.cpp
        socket_posix.cpp
        system_posix.cpp
        threading_posix.cpp           # patched: suspension through svcSetThreadActivity
        threading_switch.cpp          # filler: affinity, priority, cpu_set_t, gettid
        dynlib_switch.cpp             # an NRO has no dlopen
        exception_handler_switch.cpp  # a single __libnx_exception_handler
        fiber_switch.cpp              # context switch in assembly, without ucontext
        guest_memory_switch.cpp       # core: window, mirrors and protection by unmapping
        mapped_memory_switch.cpp      # no file mmap: read into RAM with a cap
        memory_switch.cpp             # rex::memory API on top of the core
        seh_switch.cpp                # SEH interface; faults inside SEH scopes are fatal
        switch_libc_supplement.c      # pread, pwrite and friends: newlib declares them, libnx does not
    )
elseif(UNIX)
    # Linux (the code generator only).
    target_sources(rexcore PRIVATE
        atomic_posix.cpp
        clock_posix.cpp
        dbg_posix.cpp
        dynlib_posix.cpp
        exception_handler_posix.cpp
        filesystem_posix.cpp
        fiber_posix.cpp
        mapped_memory_posix.cpp
        math_gcc.cpp
        memory_posix.cpp
        platform/console_posix.cpp
        platform/env_posix.cpp
        seh_posix.cpp
        socket_posix.cpp
        system_posix.cpp
        threading_posix.cpp
    )
endif()

# Xenos data-format layer (stateless; headers under include/rex/graphics/).
# The GPU emulation consuming it lives in the rexgpu-xenos plugin.
target_sources(rexcore PRIVATE
    ${PROJECT_SOURCE_DIR}/src/graphics/xenos.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/info.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/info_formats.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/extent.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/util.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/conversion.cpp
)

target_include_directories(rexcore PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(rexcore PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/thirdparty/cli11/include
)

if(UNIX)
    target_link_libraries(rexcore PRIVATE pthread rt dl)
endif()

target_link_libraries(rexcore
    PUBLIC
        fmt::fmt
        simde
        tomlplusplus
        spdlog::spdlog
    PRIVATE
        utf8cpp
        xxHash::xxhash
)


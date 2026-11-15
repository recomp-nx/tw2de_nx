# ReXGlue SDK - Xbox 360 Recompilation Toolkit
cmake_minimum_required(VERSION 3.25)

project(ReXGlue
    VERSION 0.10.0
    LANGUAGES C CXX
    DESCRIPTION "Xbox 360 Recompilation SDK"
)
# Floor only: bump MAJOR.MINOR for API changes. Patch is derived from git tags
# in cmake/rex_version.cmake. See docs/RELEASING.md.

#==========================================================
# Build Options
#==========================================================
# tw2de (2026-10-29): two builds only, no PC build of a game:
#   REXGLUE_PLATFORM_SWITCH  the game, for the Nintendo Switch (set by tools/switch/cmake/switch-devkitA64.cmake):
#                            the runtime, linked into the NRO; the game renders with its own renderer (Renut, deko3d)
#   REXGLUE_CODEGEN_ONLY     the code generator, rexglue (a PC program: Windows or Linux), and the part of the runtime it
#                            loads a XEX with (cmake/rexglue_codegen_only.cmake)
option(REXGLUE_PLATFORM_SWITCH             "Build for Nintendo Switch (devkitA64 + libnx; set by the toolchain file)" OFF)
option(REXGLUE_CODEGEN_ONLY                "Build only the rexglue code generator" OFF)
if(NOT REXGLUE_PLATFORM_SWITCH AND NOT REXGLUE_CODEGEN_ONLY)
    message(FATAL_ERROR "This SDK builds the Switch runtime (the switch-devkitA64.cmake toolchain) or the code generator "
                        "alone (-DREXGLUE_CODEGEN_ONLY=ON); build_switch.py does both")
endif()

# Root of the SDK source tree
set(REXGLUE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# SDK install support
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_helpers.cmake)
if(REXGLUE_PLATFORM_SWITCH)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_switch.cmake)
endif()

# The Switch build uses devkitA64's GCC (the whole runtime and, with the DEFINE_REX_FUNC fix in pch_h.inja, the
# generated code); the code generator Clang.
if(REXGLUE_PLATFORM_SWITCH)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "The Nintendo Switch build uses devkitA64's GCC (switch-devkitA64.cmake toolchain)")
    endif()
elseif(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "The code generator requires the Clang compiler. Please set CMAKE_CXX_COMPILER to clang++")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18.0")
        message(FATAL_ERROR "ReXGlue requires Clang 18 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
    endif()
endif()

# Enforce x64 architecture
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ReXGlue only supports 64‑bit architectures (x86_64 or ARM64)")
endif()

# Set C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX d)
set(CMAKE_RELWITHDEBINFO_POSTFIX rd)

# CMAKE_<CONFIG>_POSTFIX seeds libraries only, so without this every config of an
# executable installs over the last one.
function(rex_config_postfix target)
    set_target_properties(${target} PROPERTIES
        DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}"
        RELWITHDEBINFO_POSTFIX "${CMAKE_RELWITHDEBINFO_POSTFIX}")
endfunction()

if(NOT WIN32)
    # Use appropriate code model based on architecture
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        # Large code model on Linux x86_64 (from the SDK's PC builds of very large recompiled executables).
        add_compile_options(-mcmodel=large)
    else()
        add_compile_options(-mcmodel=small)
    endif()
    # Position-independent code for all targets
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Global compile options
# tw2de (2026-10-23): clang-cl (the MSVC-style driver) ignores clang's own -f / -O options unless they are written
# /clang:<option> ("unknown argument ignored in clang-cl: '-ffp-model=strict'"): the same options in that form, so a
# Windows build with clang-cl gets the floating-point model and char8_t setting the others get.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(REX_CLANG_OPT "/clang:")
else()
    set(REX_CLANG_OPT "")
endif()
# tw2de (2026-10-30): the Release optimisation level, -O3 unless the title sets another before adding the SDK
# (app/CMakeLists.txt: TW2DE_OPT, build_switch.py --opt).
if(NOT REXGLUE_RELEASE_OPT)
    set(REXGLUE_RELEASE_OPT -O3)
endif()
add_compile_options(
    -fno-strict-aliasing
    # GCC has no -ffp-model. What matters is no FP contraction: with it, GCC
    # fuses multiply-add into FMA on aarch64 and results drift from the PPC.
    $<IF:$<CXX_COMPILER_ID:GNU>,-ffp-contract=off,${REX_CLANG_OPT}-ffp-model=strict>
    $<$<CONFIG:DEBUG>:-g>
    $<$<CONFIG:DEBUG>:${REX_CLANG_OPT}-O0>
    $<$<CONFIG:RELEASE>:${REX_CLANG_OPT}${REXGLUE_RELEASE_OPT}>
    $<$<CONFIG:RELEASE>:-DNDEBUG>
    $<$<COMPILE_LANGUAGE:CXX>:${REX_CLANG_OPT}-fno-char8_t>
)

# Platform detection (supports AMD64 and ARM64)
set(REX_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")

if(REXGLUE_PLATFORM_SWITCH)
    # Horizon on aarch64. Code detects it through __SWITCH__ (rex/platform.h);
    # CMAKE_SYSTEM_NAME is Generic, so neither WIN32 nor UNIX is set.
    set(REX_PLATFORM "switch-arm64")
    # Headers Horizon lacks (netinet/ip.h, endian.h), ahead of everything.
    include_directories(BEFORE SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/switch_compat)
elseif(WIN32)
    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "win-amd64")
    elseif(REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(REX_PLATFORM "win-arm64")
    else()
        message(FATAL_ERROR "Unsupported Windows architecture: ${REX_TARGET_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_WINDOWS=1)
elseif(UNIX AND NOT APPLE)
    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "linux-amd64")
    elseif(REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(REX_PLATFORM "linux-arm64")
    else()
        set(REX_PLATFORM "linux-${REX_TARGET_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_LINUX=1)
else()
    message(FATAL_ERROR "Unsupported platform: the Switch, or Windows or Linux for the code generator.")
endif()

# Build config as compile definition (multi-config generators don't know config at configure time)
add_compile_definitions(REXGLUE_BUILD_CONFIG="$<CONFIG>")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rex_pch.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rex_version.cmake)
rex_resolve_version(REXGLUE_FULL_VERSION
    FLOOR_MAJOR ${PROJECT_VERSION_MAJOR}
    FLOOR_MINOR ${PROJECT_VERSION_MINOR})

# Strip the -id trailer (if any) for the numeric-only version.
string(REGEX REPLACE "-.*$" "" REXGLUE_NUMERIC_VERSION "${REXGLUE_FULL_VERSION}")

# Parse up to 4 numeric components for consumers that want integer access.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?" _ "${REXGLUE_NUMERIC_VERSION}")
set(REXGLUE_DERIVED_MAJOR "${CMAKE_MATCH_1}")
set(REXGLUE_DERIVED_MINOR "${CMAKE_MATCH_2}")
if(CMAKE_MATCH_4)
    set(REXGLUE_DERIVED_PATCH "${CMAKE_MATCH_4}")
else()
    set(REXGLUE_DERIVED_PATCH "0")
endif()
if(CMAKE_MATCH_6)
    set(REXGLUE_DERIVED_TWEAK "${CMAKE_MATCH_6}")
else()
    set(REXGLUE_DERIVED_TWEAK "0")
endif()

set(REXGLUE_FLOOR_VERSION
    "${REXGLUE_DERIVED_MAJOR}.${REXGLUE_DERIVED_MINOR}.${REXGLUE_DERIVED_PATCH}")

rex_version_channel(REXGLUE_VERSION_CHANNEL VERSION "${REXGLUE_FULL_VERSION}")

# Generate version header -- must run after REX_PLATFORM is set
# Timestamp is set at configure time -- intentionally not updated on every rebuild
string(TIMESTAMP REXGLUE_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/rex/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/rex/version.h
    @ONLY
)

# Output directories (multi-config appends /<Config>/ automatically)
# tw2de (2026-10-22): REX_OUTPUT_DIR for a build dir of its own (a profile-instrumented build wrote its
# libraries over the release build's, which then linked the instrumented ones).
set(REX_OUTPUT_DIR "${REXGLUE_ROOT}/out/${REX_PLATFORM}" CACHE PATH "Where the SDK's libraries and tools go")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")

# Third-party dependencies (vendored sources only)
add_subdirectory(thirdparty)

# Enable warnings after thirdparty
# tw2de (2026-10-23): clang-cl (the MSVC-style driver) reads -Wall as MSVC's /Wall, which it maps to -Weverything
# (every diagnostic clang has: -Wunsafe-buffer-usage, C++98 compatibility...). Its /W4 is clang's -Wall -Wextra.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_compile_options(/W4)
else()
    add_compile_options(
        -Wall
        -Wextra
    )
endif()
# tw2de (2026-10-23): parameters a function does not use (callbacks and interfaces with a fixed signature),
# variables and static functions left unused (often only used in some configurations or on some platforms) are
# deliberate here; the warnings only bury the others.
add_compile_options(-Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function)
# The Microsoft CRT marks standard C functions (getenv, fopen, sscanf...) deprecated in favour of its own _s versions.
if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

include_directories(${REXGLUE_ROOT}/include)

if(REXGLUE_CODEGEN_ONLY)
    include(cmake/rexglue_codegen_only.cmake)
else()
    add_subdirectory(src/core)
    add_subdirectory(src/filesystem)
    add_subdirectory(src/ui)
    add_subdirectory(src/input)
    add_subdirectory(src/audio)
    add_subdirectory(src/graphics)
    add_subdirectory(src/system)
    add_subdirectory(src/kernel)
endif()

include(cmake/rexglue_link_audit.cmake)

# Print configuration summary
message(STATUS "===========================================")
message(STATUS "ReXGlue Configuration Summary:")
message(STATUS "  Version: ${REXGLUE_FULL_VERSION} (${REXGLUE_VERSION_CHANNEL})")
message(STATUS "  Platform: ${REX_PLATFORM}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Configurations: ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Output Dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "===========================================")

# The Switch build consumes the SDK through add_subdirectory() (app/generated/rexglue.cmake): the same interface as
# find_package(). This sets REXGLUE_SHARE_DIR and makes rexglue_configure_target() available. The code generator alone
# installs and exports nothing.
if(NOT REXGLUE_CODEGEN_ONLY)
    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui" PARENT_SCOPE)
    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_helpers.cmake)
endif()

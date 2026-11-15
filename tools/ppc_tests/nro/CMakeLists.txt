# tw2de (2026-11-01): the PowerPC instruction tests as a Switch homebrew (tools/ppc_tests/build_ppc_tests.py
# configures this with the project's devkitA64 toolchain file and GEN_DIR, the generated tests).
cmake_minimum_required(VERSION 3.16)
project(tw2de_ppc_tests C CXX)

set(GEN_DIR "" CACHE PATH "The generated tests (build_ppc_tests.py's output)")
set(REXSDK_DIR "" CACHE PATH "The SDK (sdk/)")
if(NOT GEN_DIR OR NOT REXSDK_DIR)
    message(FATAL_ERROR "GEN_DIR and REXSDK_DIR are needed")
endif()

file(GLOB _chunks "${GEN_DIR}/f_*.cpp")
add_executable(tw2de_ppc_tests
    main.cpp
    stubs.cpp
    "${GEN_DIR}/regs.cpp"
    "${GEN_DIR}/fnlist.cpp"
    "${REXSDK_DIR}/src/core/ppc_estimates.cpp"
    ${_chunks})
set_target_properties(tw2de_ppc_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON)
target_include_directories(tw2de_ppc_tests PRIVATE
    "${GEN_DIR}"
    "${REXSDK_DIR}/include"
    "${REXSDK_DIR}/thirdparty/simde"
    "${REXSDK_DIR}/thirdparty/simde/simde"
    "${REXSDK_DIR}/thirdparty/fmt/include"
    "${REXSDK_DIR}/thirdparty/spdlog/include"
    "${DEVKITPRO}/libnx/include"
    "${DEVKITPRO}/portlibs/switch/include")
# The flags of the game's generated code (app/CMakeLists.txt, the Release build without LTO): what is tested is what
# the game runs. spdlog header-only (the tests' trap messages go nowhere: stubs.cpp).
target_compile_definitions(tw2de_ppc_tests PRIVATE NDEBUG SPDLOG_FMT_EXTERNAL FMT_HEADER_ONLY)
target_compile_options(tw2de_ppc_tests PRIVATE -O3 -mearly-ra=none -mno-outline-atomics -fipa-pta
    -ffp-contract=off -fno-strict-aliasing -w)
target_link_directories(tw2de_ppc_tests PRIVATE "${DEVKITPRO}/libnx/lib")
target_link_libraries(tw2de_ppc_tests PRIVATE nx stdc++ m)
target_link_options(tw2de_ppc_tests PRIVATE -pthread)

find_program(NACPTOOL nacptool HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)
find_program(ELF2NRO elf2nro HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)
add_custom_command(TARGET tw2de_ppc_tests POST_BUILD
    COMMAND "${NACPTOOL}" --create "TW2DE NX PPC tests" "tw2de" "1.0.0"
        "$<TARGET_FILE_DIR:tw2de_ppc_tests>/tw2de_ppc_tests.nacp"
    COMMAND "${ELF2NRO}" "$<TARGET_FILE:tw2de_ppc_tests>"
        "$<TARGET_FILE_DIR:tw2de_ppc_tests>/tw2de_ppc_tests.nro"
        "--nacp=$<TARGET_FILE_DIR:tw2de_ppc_tests>/tw2de_ppc_tests.nacp"
        "--romfsdir=${GEN_DIR}/romfs"
    COMMENT "Packaging tw2de_ppc_tests.nro"
    VERBATIM)

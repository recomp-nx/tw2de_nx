# plume (UnleashedRecomp-NX's Switch patches; tw2de: the deko3d back end).
# tw2de (2026-10-29): the Switch's deko3d back end only. plume's other back ends (Vulkan, D3D12, Metal), its SDL,
# D3D12 Agility SDK and Apple options, the static NVK driver and the examples are gone with the PC build. The Vulkan
# headers stay (the renderer's code names Vulkan types in branches it never takes on the console).
cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(plume)

set(PLUME_SWITCH_DEVKITPRO "/opt/devkitpro" CACHE PATH "devkitPro root for Switch builds")
set(PLUME_SWITCH_UAM_DIR "" CACHE PATH "uam source (tw2de: app/thirdparty/uam)")
message(STATUS "Plume - Building with backends: deko3d")

# Create target (its sources below: the deko3d back end)
add_library(plume STATIC plume_vulkan.h)

# Include directories
target_include_directories(plume PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/volk
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/Vulkan-Headers/include
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/VulkanMemoryAllocator/include
)

target_compile_definitions(plume PUBLIC
    __SWITCH__
    NX
    VK_USE_PLATFORM_VI_NN
)
target_include_directories(plume PUBLIC
    "${PLUME_SWITCH_DEVKITPRO}/libnx/include"
    "${PLUME_SWITCH_DEVKITPRO}/portlibs/switch/include"
)
# tw2de: no LSFG-VK; plume_lsfg_stub.cpp declines frame generation (the bridge header is next to plume's sources).
target_include_directories(plume PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_sources(plume PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plume_lsfg_stub.cpp")
target_compile_features(plume PUBLIC cxx_std_20)
# No Vulkan driver: the parent links stubs that answer "not available" (sdk/src/ui/switch_vulkan_stubs.c).
target_link_libraries(plume PUBLIC zstd z nx)

# tw2de (2026-10-17): the deko3d back end (plume_deko3d*.cpp, tw2de docs/deko3d-renderer.md): devkitPro's
# deko3d and uam (PLUME_SWITCH_UAM_DIR: tw2de vendors SwitchWakerHD's patched copy, whose CMake builds the static
# uamlib for the Switch when NX_ROOT is set).
# tw2de (2026-10-30): uam is left out of link-time optimisation (TW2DE_LTO): compiled to machine code at its
# own -O2, without -fipa-pta, as in the builds without LTO. With it, Mesa's GLSL front end came out wrong (the O3 LTO
# build's pre-pass: every variant failed in the header's saturate / select, "parameter `x` must be a shader input";
# the same shaders compiled with the build without LTO). uam runs for shader compiles only, not per frame.
if(NOT EXISTS "${PLUME_SWITCH_UAM_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "plume's deko3d back end needs PLUME_SWITCH_UAM_DIR (uam's source): ${PLUME_SWITCH_UAM_DIR}")
endif()
if(NOT TARGET uamlib)
    set(NX_ROOT "${PLUME_SWITCH_DEVKITPRO}/libnx")
    add_subdirectory("${PLUME_SWITCH_UAM_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/uam")
endif()
if(TW2DE_LTO)
    target_compile_options(uamlib PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fno-lto> $<$<COMPILE_LANGUAGE:C,CXX>:-fno-ipa-pta>)
endif()
target_sources(plume PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/plume_deko3d.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/plume_deko3d_commands.cpp")
target_compile_definitions(plume PUBLIC PLUME_DEKO3D=1)
target_include_directories(plume PRIVATE "${PLUME_SWITCH_UAM_DIR}")
# PLUME_DEKO3D_DEBUG: devkitPro's debug deko3d (libdeko3dd.a), which checks every call's input and reports through
# the device's debug callback (plume_deko3d's log): for tests, slower.
option(PLUME_DEKO3D_DEBUG "Link the debug deko3d library (input checks)" OFF)
if(PLUME_DEKO3D_DEBUG)
    set(plume_deko3d_lib "${PLUME_SWITCH_DEVKITPRO}/libnx/lib/libdeko3dd.a")
    # and guard bytes after every memory block, checked at each submission (plume_deko3d.cpp makeBlock)
    target_compile_definitions(plume PRIVATE PLUME_DEKO3D_GUARDS=1)
else()
    set(plume_deko3d_lib "${PLUME_SWITCH_DEVKITPRO}/libnx/lib/libdeko3d.a")
endif()
target_link_libraries(plume PUBLIC uamlib "${plume_deko3d_lib}" nx)
message(STATUS "plume: deko3d back end with uam from ${PLUME_SWITCH_UAM_DIR}")

# rexui

# tw2de (2026-10-29): the Switch only: libnx's window and UI loop (no SDL3, no Vulkan or D3D12 presenter).
set(REXUI_SOURCES
    graphics_upload_buffer_pool.cpp
    graphics_util.cpp
    image_decode.cpp
    immediate_drawer.cpp
    keybinds.cpp
    menu_item.cpp
    presenter.cpp
    renderdoc_api.cpp
    window.cpp
    windowed_app.cpp
    windowed_app_context.cpp
    window_switch.cpp
    windowed_app_context_switch.cpp
)

add_library(rexui OBJECT ${REXUI_SOURCES})
add_library(rex::ui ALIAS rexui)

rex_target_pch(rexui <rex/math.h>)

target_include_directories(rexui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexui PRIVATE
    ${PROJECT_BINARY_DIR}/include  # rex/version.h (generated)
    ${REXGLUE_ROOT}/src
    ${REXGLUE_ROOT}/thirdparty/stb  # stb_image.h for icon decoding
)

target_link_libraries(rexui PUBLIC rexcore)
target_link_libraries(rexui PRIVATE utf8cpp)

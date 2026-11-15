/**
 * @file    gpu/overlay/app_integration.h
 * @brief   What the app needs to run the native renderer in the SDK's detached
 *          ("bring your own renderer") mode.
 *
 * tw2de (2026-10-22): no overlay any more (Dear ImGui is gone: reNut's overlay, the SDK's menus and the pump that
 * presented them while the guest was not presenting).
 */
#pragma once

namespace rex {
struct RuntimeConfig;
} // namespace rex

namespace renut::gpu {

// OnPreSetup: no SDK graphics system and no GPU plugin.
void ConfigureRuntime(rex::RuntimeConfig &config);

// Once the window exists: creates the host device.
bool StartRenderer();

// OnWindowPixelSizeChanged.
void OnWindowResized();

// OnShutdown.
void ShutdownRenderer();

} // namespace renut::gpu

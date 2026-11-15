/**
 * @file    gpu/overlay/app_integration.cpp
 */
#include "renut_engine/gpu/overlay/app_integration.h"

#include <rex/runtime.h>

#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/log.h"

namespace renut::gpu {

void ConfigureRuntime(rex::RuntimeConfig &config) {
  config.graphics = nullptr;
  config.gpu_plugin.clear();
}

bool StartRenderer() {
  if (!Video::CreateHostDevice()) {
    RGPU_ERROR("Renderer initialisation failed");
    return false;
  }
  return true;
}

void OnWindowResized() {
  Video::RequestResize();
}

void ShutdownRenderer() {
  Video::BeginShutdown();
}

} // namespace renut::gpu

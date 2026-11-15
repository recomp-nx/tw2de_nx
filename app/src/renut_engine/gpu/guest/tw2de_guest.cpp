// tw2de: the game-specific guest glue reNut's renderer asks for (tw2de's guest/widescreen.cpp and
// guest/photo_capture.cpp are Nuts & Bolts code). The game renders its 3D at 1280x720 on the 360.
#include "renut_engine/gpu/guest/widescreen.h"

namespace renut::gpu {

uint32_t GuestScreenWidth() {
  return 1280;
}

}  // namespace renut::gpu

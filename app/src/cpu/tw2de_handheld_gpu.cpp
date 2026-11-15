// tw2de (2026-10-25, tw2de_handheld_gpu_460): the game's handheld performance configuration: GPU 460.8 MHz.
//
// Handheld, Horizon gives an application GPU 307.2 MHz unless it asks for another of its performance configurations
// (apm, the API games use: no overclock). 0x92220008 is GPU 460.8 MHz with the memory at 1331.2 MHz, handheld's
// usual memory clock (its sibling 0x92220007 raises the memory to 1600 MHz: not used). The configuration is set for
// the handheld performance mode (ApmPerformanceMode_Normal) whether the console is docked now or not: Horizon uses
// it whenever the console is handheld, and docked play keeps its own. It lasts while the software runs.
// (sdk/src/ui/switch_apm.cpp found these numbers: 0x00020003 is the default, GPU 307.2 / memory 1331.2.)
#include "cpu/tw2de_handheld_gpu.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(tw2de_handheld_gpu_460, true, "Game/Performance",
                    "Handheld: ask Horizon for its GPU 460.8 MHz performance configuration (memory unchanged) instead "
                    "of the default 307.2 MHz")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de {

void HandheldGpuInit() {
#ifdef __SWITCH__
  if (!REXCVAR_GET(tw2de_handheld_gpu_460))
    return;
  constexpr uint32_t kGpu460 = 0x92220008u;  // GPU 460.8 MHz, memory 1331.2 MHz
  Result rc = apmInitialize();
  if (R_FAILED(rc)) {
    REXLOG_WARN("[apm] apm could not be opened (0x{:X}): the handheld GPU clock stays the default", rc);
    return;
  }
  u32 before = 0;
  apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &before);
  rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, kGpu460);
  if (R_SUCCEEDED(rc))
    REXLOG_INFO("[apm] handheld: performance configuration 0x{:08X} (GPU 460.8 MHz, memory 1331.2 MHz), was 0x{:08X}",
                kGpu460, before);
  else
    REXLOG_WARN("[apm] handheld: configuration 0x{:08X} refused (0x{:X}): stays 0x{:08X}", kGpu460, rc, before);
  // The apm service stays open: the configuration belongs to this session of it.
#endif
}

}  // namespace tw2de

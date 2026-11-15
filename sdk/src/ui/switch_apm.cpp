/*
 * Horizon console performance profile (apm).
 *
 * Why
 *   The GPU runs at 307.2 MHz in handheld mode and it is one of the two walls of the port (the other
 *   is the CPU, with three cores at 85-99 %). Nintendo later released the 460.8 MHz handheld profile
 *   precisely for games that need it, and this one does: 25 FPS with the GPU at its limit. It is not
 *   an overclock: no KIP or sysmodule is touched, it is requested through Horizon's official API
 *   (apm), the same one any commercial game uses. That is why not every game requests it: those that
 *   do not need it get longer battery life.
 *
 *   307.2 -> 460.8 MHz is +50 % of GPU.
 *
 * Memory is not touched
 *   The first version raised the memory from 1331.2 to 1597.5 MHz by accident: apm does not take MHz,
 *   it takes a configuration number, and the high-GPU configurations come in two flavors, with EMC
 *   1331.2 and with EMC 1600. The 1600 one got in. A clock overlay showed it, and a commercial game
 *   runs at GPU 460.8 with MEM 1331.2: so that combination exists and it is the one we want.
 *
 *   Raising the memory clock uses more battery and heat without giving us anything: our own
 *   measurements say bandwidth is at 27 % and what saturates is the fragment ALU.
 *
 * The three rounds it took, and what each one taught
 *
 *   1. It applied the configuration and read the clocks on the next line. The log said "the
 *      memory stays at 1331.2" and the session profile recorded 1600.0 in every sample. They
 *      did not contradict each other: it was checked too early. apm hands the change to pcv and
 *      the clocks are applied later, so the immediate read returned the old value. The same bug
 *      left dead a fix that only lowered the EMC by hand "if it is detected that it went up": it
 *      was never detected.
 *
 *   2. A 250 ms wait plus a once-per-second watchdog that lowered the EMC with clkrst. The log said
 *      the two things that needed knowing:
 *        - after 250 ms the memory was still at 1331.2 (so the raise takes between 0.25 and ~1 s,
 *          and 250 ms was not enough either);
 *        - the watchdog found it at 1600 and lowered it eight times with clkrst; all eight were
 *          accepted and all eight were reverted by the system. pcv reimposes the EMC of the active
 *          apm configuration, and clkrst cannot fight that.
 *
 *   3. So the problem was never how to lower the memory, but which configuration is requested.
 *      0x92220007 is "GPU 460.8 + EMC 1600", and its sibling 0x92220008 is "GPU 460.8 + EMC
 *      1331.2": that is the right one, and it was missing from the list (a nonexistent 0x92220006
 *      was there instead). Now:
 *        1. the table has the real IDs, sorted with the low EMC first;
 *        2. the check is a 1.5 s poll (StillMemoryDuring), not a single read;
 *        3. the watchdog stays as a safety net, in case the system reimposes the EMC on a mode
 *           change or when waking from sleep.
 *
 *   tw2de (2026-10-21): the clkrst checks (2. and 3. above) are gone: clkrst sessions are few system-wide and
 *   overlays such as Status Monitor need them. The corrected table is trusted, and the RAM-1600 entries are only
 *   taken when allowed.
 *
 *   It only ever lowers back to what the console had at the start: nothing is ever raised above
 *   that. And if this firmware did not have the "high GPU + unchanged RAM" pair, the GPU is left
 *   as it was rather than raising the RAM; masseffect_switch_ram_1600 opts into that trade.
 */

#include "rex/ui/switch_apm.h"

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <atomic>
#include <cstdio>

namespace {

std::atomic<int> g_request{0};
std::atomic<bool> g_done{false};
/*
 * The escape hatch. By default no configuration that raises the RAM clock is accepted, even if that
 * means staying with the low GPU clock. If the firmware turned out not to have the "high GPU +
 * unchanged RAM" pair, setting this to true takes the RAM 1600 one anyway.
 */
std::atomic<bool> g_allow_ram_1600{false};

struct Candidate {
  int mhz;
  uint32_t config;
  int emc;  // log only: what Horizon's table says this configuration sets
};
/*
 * The correct table. The first two are confirmed by a console log:
 *
 *   0x00020003  GPU 307.2  EMC 1331.2   <- "starting point: config 0x00020003 ... memory 1331.2"
 *   0x92220007  GPU 460.8  EMC 1600     <- "SET 0x92220007" and the memory ended at 1600
 *
 * and they match Horizon's PerformanceConfiguration table exactly. The one we want is the sibling of
 * the last one, 0x92220008 = GPU 460.8 with EMC 1331.2, which is exactly the pair a commercial game
 * uses. An earlier list lacked it (it had 0x92220006, which does not exist), so the 1600 one always
 * got in.
 *
 * They are sorted by EMC: first the one that does not touch the memory. Requesting one that does not
 * exist returns an error and changes nothing, so it is enough to list them and let the check decide.
 */
constexpr Candidate kCandidate[] = {
    {460, 0x92220008u, 1331},  // GPU 460.8 + EMC 1331.2: the right one
    {460, 0x92220007u, 1600},  // GPU 460.8 + EMC 1600: raises the RAM clock
    {384, 0x00020004u, 1331},  // GPU 384 + EMC 1331,2
    {384, 0x00010000u, 1600},  // GPU 384 + EMC 1600
};


}  // namespace

extern "C" void RexSwitchApmRequestGpuMhz(int mhz) {
  g_request.store(mhz, std::memory_order_relaxed);
}

extern "C" void RexSwitchApmAllowRam1600(int allow) {
  g_allow_ram_1600.store(allow != 0, std::memory_order_relaxed);
}

extern "C" void RexSwitchApmApply(void) {
  const int mhz = g_request.load(std::memory_order_relaxed);
  if (mhz <= 0 || g_done.exchange(true)) {
    return;
  }
  ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
  if (R_FAILED(apmGetPerformanceMode(&mode))) {
    std::fprintf(stderr, "[apm] could not read the performance mode; nothing is touched\n");
    return;
  }
  if (mode != ApmPerformanceMode_Normal) {
    std::fprintf(stderr, "[apm] the console is docked (mode %d): nothing is requested\n", int(mode));
    return;
  }
  // tw2de (2026-10-21): no clkrst (its sessions are few and overlays need them): the configuration is chosen from
  // the corrected table alone, the one that keeps the memory clock first; one that raises it only when allowed.
  const bool allow_1600 = g_allow_ram_1600.load(std::memory_order_relaxed);
  for (const Candidate& c : kCandidate) {
    if (c.mhz != mhz || (c.emc > 1331 && !allow_1600)) {
      continue;
    }
    if (R_SUCCEEDED(apmSetPerformanceConfiguration(mode, c.config))) {
      std::fprintf(stderr, "[apm] SET configuration 0x%08X (table: GPU %d MHz, memory %d MHz)\n", c.config, c.mhz,
                   c.emc);
      return;
    }
  }
  std::fprintf(stderr, "[apm] no %d MHz configuration was accepted; the console is left as it was\n", mhz);
}

extern "C" void RexSwitchApmWatch(void) {}

#else   // !REX_PLATFORM_SWITCH
extern "C" void RexSwitchApmRequestGpuMhz(int) {}
extern "C" void RexSwitchApmAllowRam1600(int) {}
extern "C" void RexSwitchApmApply(void) {}
extern "C" void RexSwitchApmWatch(void) {}
#endif  // REX_PLATFORM_SWITCH


extern "C" void RexSwitchApmCpuBoost(int mode) {
  if (mode <= 0) return;
  const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode(mode));
  std::fprintf(stderr, "[apm] appletSetCpuBoostMode(%d) -> 0x%X\n", mode, unsigned(rc));
}

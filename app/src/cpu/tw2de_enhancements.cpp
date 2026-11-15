// tw2de: the enhancement switches reNut's renderer reads (tw2de's framework, cut down: this port has none of
// tw2de's CPU enhancements yet, only the renderer's own keys, which each have their own cvar).
#include "cpu/tw2de_enhancements.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread/mutex.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

// tw2de (2026-10-22): the SDK's Switch profiler reads this flag by name (switch_perf.cpp), and only a title that
// defines it can set it from tw2de.toml; without it, only the stacks_profile.flag file in log/ turned sampling on.
REXCVAR_DEFINE_BOOL(tw2de_show_compile_notice, true, "tw2de",
                    "Switch: \"Compiling shaders...\" in small type at the top left while a shader the game waits for "
                    "is compiled, and for a moment after");
// tw2de (2026-10-23): the achievement popup (renut_engine/gpu/backend/achievement_popup.cpp).
REXCVAR_DEFINE_BOOL(tw2de_achievement_popup, true, "tw2de",
                    "Switch: \"Achievement unlocked\" with the achievement's name at the top left of the picture when "
                    "the game unlocks one (in the game's language: English, German, French, Spanish or Italian)");
REXCVAR_DEFINE_DOUBLE(tw2de_achievement_popup_seconds, 5.0, "tw2de",
                      "Switch: how long the achievement popup stays up, in seconds")
    .range(1.0, 30.0);
REXCVAR_DEFINE_BOOL(tw2de_achievement_popup_test, false, "tw2de",
                    "Switch: a test popup shortly after start (the first achievement the game has, or a sample in the "
                    "game's language), to check the popup and its letters. Nothing is unlocked");
REXCVAR_DEFINE_INT32(tw2de_guest_atomic_lock, 0, "Game/Enhancements",
                     "The game's interrupt-disabled regions around its atomic lwarx/stwcx. pairs (mfmsr / mtmsrd): "
                     "0 = no lock (the pair is a host compare-and-swap, atomic on its own; the vfx-qix-effect workers "
                     "spent most of their time queued on the lock), 1 = a lock of their own, 2 = the SDK's global "
                     "critical region, shared with the kernel's object table, audio and file system (as before, Xenia's), "
                     "3 = not each other, but the global critical region's holder keeps them out (2's exclusion "
                     "without its cost: 0 and 1 made the loads after a skipped cutscene take a minute)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(switch_profile_stacks, false, "tw2de",
                    "Switch: the profiler samples every thread's stack each millisecond (log/rex_profile.log "
                    "shows where each thread spends its time or waits). Slows the game; for diagnostics");
REXCVAR_DEFINE_BOOL(switch_profile_report, false, "tw2de",
                    "Switch: every 10 s, each thread's CPU time, the faults and the system call counters into "
                    "log/rex_profile.log (on with switch_profile_stacks too). The overlay's FPS works without it");
REXCVAR_DEFINE_BOOL(tw2de_cpu_enhancements, true, "Game/Enhancements",
                    "Master switch of the CPU enhancements (each also has its own key). false = all of them off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_noncpu_enhancements, true, "Game/Enhancements",
                    "Master switch of the non-CPU enhancements: GPU, driver and storage side (each also has its own "
                    "key). false = all of them off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_cpu_verify, false, "Game/Enhancements",
                    "Verify configuration: every enhancement that can checks itself against the original path on "
                    "every use. Slower; for testing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de {

bool EnhancementActive(EnhancementGroup group, bool own_key) {
  return own_key && (group == EnhancementGroup::kCpu ? REXCVAR_GET(tw2de_cpu_enhancements)
                                                     : REXCVAR_GET(tw2de_noncpu_enhancements));
}

void ApplyMasterSwitches() {}

void LogEnhancements() {
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[enhancements] cpu {} non-cpu {} verify {}", REXCVAR_GET(tw2de_cpu_enhancements),
              REXCVAR_GET(tw2de_noncpu_enhancements), REXCVAR_GET(tw2de_cpu_verify));
  // tw2de (2026-10-22): before the module launches (OnPostSetup), so no region is open when it changes.
  rex::thread::guest_atomic_lock_mode = REXCVAR_GET(tw2de_guest_atomic_lock);
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[enhancements] guest atomic regions: {}",
              rex::thread::guest_atomic_lock_mode == 0   ? "no lock (tw2de_guest_atomic_lock 0)"
              : rex::thread::guest_atomic_lock_mode == 1 ? "own lock (tw2de_guest_atomic_lock 1)"
              : rex::thread::guest_atomic_lock_mode == 2 ? "global critical region (tw2de_guest_atomic_lock 2)"
                                                         : "kept out by the global critical region's holder (tw2de_guest_atomic_lock 3)");
}

void CodegenChecksInit() {}
void FormsSelfCheck() {}

}  // namespace tw2de

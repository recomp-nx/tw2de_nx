// tw2de: settings and hooks reNut's renderer shares with tw2de's PM4 renderer (app/src/me there), which this
// port does not build. Same names and defaults as tw2de's definitions (me/masseffect/masseffect_native_draws.cpp).
#include <rex/cvar.h>

#include "profiler/profiler.h"

REXCVAR_DEFINE_INT32(tw2de_background_compile_core, 2, "Game",
                     "Switch: the core the background pipeline/shader compile threads run on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_gpu_blend_skip, true, "Game",
                    "GPU: transparent pixels whose blend would leave the target unchanged are discarded before the blend")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_gpu_tess_grid, true, "Game",
                    "GPU: tessellated triangle patches drawn indexed, one vertex invocation per grid point")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_gpu_robustness_off, true, "Game",
                    "GPU: pipelines created with buffer and image robustness disabled")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_gpu_bool_spec, true, "Game",
                    "GPU: pipelines specialised on the bool constants their shaders test")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The guest-store card marks come from a post-codegen step (tools/store_cards.py), which tools/post_codegen_switch.py
// runs (tw2de, 2026-10-24); the native overrides and the SDK's host writers mark theirs. Weak and false here: the
// pass defines it true in the generated tw2de_init.cpp, so the renderer trusts the cards only when the code that
// marks them is what was built (generated code without the pass: this one, and the stream checks hash as before).
extern "C" __attribute__((weak)) const bool g_tw2de_store_cards = false;

namespace tw2de {
// The post-codegen self-check report (tw2de's cpu/tw2de_codegen_checks.cpp); no such steps here.
void CodegenChecksReport(int) {}
}  // namespace tw2de

// tw2de (2026-10-24): profiler/profiler.cpp is built now (SetGpuFrameMs, SetRenderThreadMs, RecordHostPresent there).

// tw2de's PM4 renderer keys the shared code still declares (NVK tuning, ring/prewarm cores); defaults as there.
REXCVAR_DEFINE_BOOL(tw2de_gpu_nvk_draw_contract, true, "Game", "NVK per-draw contract lookup (unused here)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_cbufs, true, "MASSEFFECT", "NVK dirty cbuf slots only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_dynamic, true, "MASSEFFECT", "NVK dynamic state groups by dirt")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_emission, true, "MASSEFFECT", "NVK draw command in one go")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_preload, true, "MASSEFFECT", "NVK cache hints before a draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_set4_differences, true, "MASSEFFECT", "NVK descriptor set 4 by differences")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_prewarm_threads, 1, "Game", "Threads that prewarm the pipeline list")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_ring_core, 1, "Game", "Switch: the core the render thread is pinned to (-1 = none)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_nvk_measure, 64, "MASSEFFECT", "NVK measuring by parts, 1 in N calls")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#include <rex/platform.h>
#if !REX_PLATFORM_SWITCH
// The console profiler's counters (sdk/src/ui/switch_perf.cpp on the Switch): nothing on the PC.
extern "C" void RexSwitchPerfCount(unsigned) {}
#endif

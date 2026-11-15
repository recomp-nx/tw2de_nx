// TW2 (2026-11-12): what the renderer and the boot menu still use from the FNXF files that hook FF-only code
// (left out of the build until ported to TW2): the same names, the game's own behaviour.
//   - render resolution: the game's own scene size (no start-up hook yet), so RenderHeight() is 720 whatever the
//     boot menu set; the cvars stay so the menu and the toml keep working.
//   - the load-fade fix (an FF XIII-LR camera bug): never holds.
//   - the language pack's texts (hooked FF text lookups): nothing to install.
#include <rex/cvar.h>

#include <cstdint>

REXCVAR_DEFINE_BOOL(tw2de_render_720p, false, "Game/Enhancements",
                    "The game's 3D scene at 1280x720 (TW2: always, the scene size is not changed yet)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_render_height, 720, "Game/Enhancements",
                     "The game's 3D scene height (TW2: not applied yet, the game's own size)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de {
uint32_t RenderHeight() { return 720u; }
void TextsInit() {}
}  // namespace tw2de

bool FnxfXiiiLrLoadFadeHold() { return false; }

// TW2 (2026-11-14): the size of the game's 3D scene.
//
// The game creates its main viewport at 1280x672 in one place: sub_82F8D610 calls the renderer's virtual at +0x28
// (sub_82EF0328, vtable 0x82276248 of the object at 0x8355B94C) with r7 = 1280, r8 = 672 (li r8,0x2a0 at 0x82F8D654:
// the only 672 next to a 1280 in the code). 672 lines is what lets two colour targets and the depth of the G-buffer
// fit the Xbox 360's 10 MB of EDRAM (3 x 1280 x 672 x 4 = 10.3 million bytes); nothing of that limits this port.
// sub_82EF0328(renderer, r4, r5, title, width, height, fullscreen) first calls sub_82EED7B8(renderer, width, height),
// then builds the viewport object (sub_82EDF6F8) with them.
//
// tw2de_scene_width / tw2de_scene_height: that one call's size replaced (0: the game's own). A first step for the
// boot menu's Resolution row; what the rest of the frame does with another size is to be seen on the console.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <cstdint>

REXCVAR_DEFINE_INT32(tw2de_scene_width, 0, "Game/Enhancements",
                     "The width of the game's 3D scene instead of its own 1280 (0: the game's; with "
                     "tw2de_scene_height)")
    .range(0, 1920)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_scene_height, 0, "Game/Enhancements",
                     "The height of the game's 3D scene instead of its own 672 (0: the game's; with tw2de_scene_width)")
    .range(0, 1080)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82EF0328);

extern "C" REX_FUNC(sub_82EF0328) {
  const uint32_t width = ctx.r7.u32, height = ctx.r8.u32;
  const int32_t w = REXCVAR_GET(tw2de_scene_width), h = REXCVAR_GET(tw2de_scene_height);
  if (w > 0 && h > 0 && width == 1280 && height == 672) {
    ctx.r7.u64 = uint32_t(w);
    ctx.r8.u64 = uint32_t(h);
  }
  REXLOG_INFO("[scene] the game's viewport: {}x{} asked, {}x{} made", width, height, ctx.r7.u32, ctx.r8.u32);
  __imp__sub_82EF0328(ctx, base);
}

// tw2de (2026-10-25, tw2de_render_720p): the game's 3D scene at 1280x720 instead of 1024x576 (the boot menu's
// "Resolution").
//
// sub_8272D760 builds the engine's start-up parameters (the CDev framework's, extended by GameMain's sub_82201C90 and
// kept as its singleton at 0x82AAA004): +24/+28 = 1024 x 576, +36/+40 = 1280 x 720, the only place in the code where
// these two sizes are set together. Taken to be the scene's render size and the output's (the game's UI is 1280x720
// and its upscale draw scales the 1024x576 picture to 1280x720). Here +24/+28 become 1280 x 720 right after it.
// A first step, to be confirmed on the console: the log line says what was there and what was written; if the scene
// is still drawn at 1024x576 (the upscale draw's c0 in the log's [upscale] line stays 1024 x 576), the render size is
// taken from elsewhere.
#include <algorithm>
#include <fmt/format.h>
#include <string>
#include <chrono>
#include <mutex>
#include <vector>
#include <rex/system/xmemory.h>
#include <rex/system/kernel_state.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <cstdint>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(tw2de_render_720p, false, "Game/Enhancements",
                    "Experimental: the game's 3D scene at 1280x720 instead of its own 1024x576 (the boot menu's "
                    "\"Resolution\"; the upscaler is not used then). About 56% more GPU work a frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tw2de (2026-11-07): the scene's height (16:9: 576 = 1024x576, the game's; 720, 900, 1080). Over render_720p.
// tw2de (2026-11-10): FF XIII-2's own scene is 1280x720 (720, the default); 576 draws it at 1024x576 (less GPU
// work, the game's upscale to the output as in FF XIII). 900 and 1080 read as 720 until their hooks (the EDRAM tiling
// and the render pool below, FF XIII's addresses) are found in FF XIII-2.
REXCVAR_DEFINE_INT32(tw2de_render_height, 720, "Game/Enhancements",
                     "The game's 3D scene height, 16:9: 720 (1280x720, the game's own) or 576 (1024x576, less GPU "
                     "work, upscaled to the output)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tw2de {
// The scene's height in effect (tw2de_render_height).
uint32_t RenderHeight() {
  const int32_t h = REXCVAR_GET(tw2de_render_height);
  // tw2de (2026-11-11): 900 and 1080 too, as FNXF XIII-2 (the start-up hook below sets the scene's size; the
  // render pool grown in sub_822FA438's hook).
  if (h == 900 || h == 1080)
    return uint32_t(h);
  return h == 576 && !REXCVAR_GET(tw2de_render_720p) ? 576u : 720u;
}
}  // namespace tw2de

// tw2de (2026-11-10, LR): LR's start-up parameters are sub_82AC22E0 (FF XIII-2's sub_82AC22E0: the same stores
// of 1280 x 720 at +24/+28 and +36/+40, then its call sub_82AC2228, FF XIII-2's sub_82AC2228; LR's structure is
// shorter after +0x30, which this hook does not touch).
// tw2de (2026-11-10): FF XIII-2's start-up parameters, sub_82913BA8 (FF XIII's sub_8272D760: the same stores,
// its tail call sub_82913AE8 FF XIII's sub_8272D6A8): +24/+28 = 1280 x 720 and +36/+40 = 1280 x 720 here. With the
// scene at 576 (tw2de_render_height) +24/+28 become 1024 x 576 right after it, as FF XIII had them. A hypothesis
// until the console shows it: the log's [resolution] line says what was there and what was written, and the
// upscale draw's size whether the game drew its scene at that size.
REX_EXTERN(__imp__sub_82AC22E0);

namespace {
uint32_t LoadParam(const uint8_t *base, uint32_t ea) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea));
}
void StoreParam(uint8_t *base, uint32_t ea, uint32_t v) {
  *reinterpret_cast<uint32_t *>(base + ea) = __builtin_bswap32(v);
}
}  // namespace

extern "C" REX_FUNC(sub_82AC22E0) {
  const uint32_t params = ctx.r3.u32;
  __imp__sub_82AC22E0(ctx, base);
  if (!params)
    return;
  const uint32_t w = LoadParam(base, params + 24), h = LoadParam(base, params + 28);
  const uint32_t out_w = LoadParam(base, params + 36), out_h = LoadParam(base, params + 40);
  const uint32_t rh = tw2de::RenderHeight(), rw = rh * 16 / 9;
  if (rh != 720 && w == 1280 && h == 720 && out_w == 1280 && out_h == 720) {
    StoreParam(base, params + 24, rw);
    StoreParam(base, params + 28, rh);
    // tw2de (2026-11-11): +36/+40 too, as FNXF XIII-2 (its fnxf_xiii_2_test_scene_output). The renderer
    // (0x83147AB4) keeps +20 the display's size, +32 these +36/+40, +44 the scene's +24/+28 (seen on the console:
    // 1280x720 / 1280x720 / 1024x576 with only +24/+28 set): LR draws its scene at +32, so the scene stayed 1280x720
    // (its depth target 1280x720 in the field). The GUI's layout hooks below give it the display's size again.
    StoreParam(base, params + 36, rw);
    StoreParam(base, params + 40, rh);
    REXLOG_INFO("[resolution] start-up parameters at {:08X}: scene {}x{} -> {}x{} (output {}x{} -> {}x{})", params, w,
                h, rw, rh, out_w, out_h, rw, rh);
  } else {
    REXLOG_INFO("[resolution] start-up parameters at {:08X}: scene {}x{}, output {}x{}", params, w, h, out_w, out_h);
  }
}

// tw2de (2026-11-07): GameMain's start-up parameters (sub_82201C90, right after sub_8272D760) give the game's memory
// pools their sizes: +76..+136 its heaps (summed against +140), +144 0xCC00000 (204 MB) of write-combined physical
// memory (MmAllocatePhysicalMemoryEx, protect 0x20000404: textures and render targets), +148 0x600000. A 1920x1080
// scene's targets did not fit in +144 and the field's load waited forever for room (no allocation failed in the
// kernel; a 1600x900 scene fits). Grown before sub_821F37F8 (the next call, then sub_82200BD8) makes the pools.
// 64 MB more left too little guest physical memory for Renut's own 64 MB record heap ("Reserving 67108864 bytes of
// guest physical memory failed"); 32 MB loads the field. 1600x900 needs none (not grown: the memory stays free).
REXCVAR_DEFINE_INT32(tw2de_render_pool_extra_mb, 32, "Game/Enhancements",
                     "With a 1920x1080 scene (tw2de_render_height): this many MB more for the game's render "
                     "target and texture pool (its start-up parameter +144, 204 MB)")
    .range(0, 160)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tw2de (2026-11-11): LR's render pool. Its pool set-up sub_822FA438 (FF XIII-2's sub_8225E4E0) passes its
// parameters' +0x64 (FF XIII-2's +156, 0xEF00000 there) to the allocator sub_82AB8A08 (FF XIII-2's sub_82911730).
// Logged; with a scene above 1280x720 grown by tw2de_render_pool_extra_mb when it holds a pool-like size
// (128-512 MB), as FNXF XIII-2 does.
REX_EXTERN(__imp__sub_822FA438);
extern "C" REX_FUNC(sub_822FA438) {
  // tw2de (2026-11-11): the call order differs from FF XIII-2's, and +0x64 held 4 MB on the console: every size
  // the set-up passes is logged, and the largest in 128-512 MB (the render target and texture pool; 239 MB in FF XIII-2)
  // grown with a scene above 1280x720.
  const uint32_t params = ctx.r3.u32;
  if (params) {
    static constexpr uint32_t kOffsets[] = {0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80, 0x84, 0x88, 0x8C};
    std::string line;
    uint32_t best = 0, best_off = 0;
    for (uint32_t o : kOffsets) {
      const uint32_t v = LoadParam(base, params + o);
      line += fmt::format(" +{:X}={}MB", o, v >> 20);
      if (v >= (128u << 20) && v <= (512u << 20) && v > best) {
        best = v;
        best_off = o;
      }
    }
    REXLOG_INFO("[resolution] pool set-up sizes:{}", line);
    const uint32_t extra = uint32_t(REXCVAR_GET(tw2de_render_pool_extra_mb)) << 20;
    if (best && extra && tw2de::RenderHeight() > 720) {
      StoreParam(base, params + best_off, best + extra);
      REXLOG_INFO("[resolution] the pool at +{:X}: {} MB (+{} MB for the {}-line scene)", best_off, (best + extra) >> 20,
                  extra >> 20, tw2de::RenderHeight());
    }
  }
  __imp__sub_822FA438(ctx, base);
}

// tw2de (2026-11-10): FNXF XIII-2's 2D layout and scene-target fixes for another scene size (its
// fnxf_xiii_2_render_resolution.cpp), ported. LR's renderer object is at 0x83147AB4 (XIII-2's 0x82EA1784: the getters
// sub_82732C60 / sub_82732C70 read its +32 / +36, as XIII-2's sub_825BEFA8 / sub_825BEFB8); its GUI converts the
// 1280x720 layout in sub_827333E0 / sub_827334A0 (XIII-2's sub_825BEE00 / sub_825BEEA8; LR's call the getters).
REXCVAR_DEFINE_INT32(tw2de_layout_hooks, 319, "Game/Diagnostics",
                     "Diagnostic (2026-11-11): which GUI layout-size hooks run at a scene other than 1280x720: 1 "
                     "sub_827333E0, 2 sub_827334A0, 4 the getters sub_82732C60 / 70, 8 sub_82739610, 16 the GUI's scissor (sub_82668D08), 32 the battle markers' projection, 256 sub_825547E8 (the battle markers)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace {
constexpr uint32_t kRendererObject = 0x83147AB4u;
// tw2de (2026-11-11, diagnostic): the renderer object's first words, once (which of them hold the scene size).
void LogRendererObject(const uint8_t *base) {
  static bool logged = false;
  const uint32_t r = LoadParam(base, kRendererObject);
  if (logged || !r)
    return;
  logged = true;
  std::string words;
  for (uint32_t o = 0; o < 128; o += 4)
    words += fmt::format(" +{}={}", o, LoadParam(base, r + o));
  REXLOG_INFO("[resolution] renderer object {:08X}:{}", r, words);
}
template <void (*Original)(PPCContext &, uint8_t *), uint32_t kBit>
void WithLayoutSize(PPCContext &ctx, uint8_t *base) {
  LogRendererObject(base);
  static const uint32_t mask = uint32_t(REXCVAR_GET(tw2de_layout_hooks));
  const uint32_t r = tw2de::RenderHeight() != 720 && (mask & kBit) ? LoadParam(base, kRendererObject) : 0;
  if (!r) {
    Original(ctx, base);
    return;
  }
  const uint32_t w = LoadParam(base, r + 32), h = LoadParam(base, r + 36);
  StoreParam(base, r + 32, LoadParam(base, r + 20));
  StoreParam(base, r + 36, LoadParam(base, r + 24));
  Original(ctx, base);
  StoreParam(base, r + 32, w);
  StoreParam(base, r + 36, h);
}
}  // namespace

REX_EXTERN(__imp__sub_827333E0);
REX_EXTERN(__imp__sub_827334A0);
REX_EXTERN(__imp__sub_82732C60);
REX_EXTERN(__imp__sub_82732C70);
extern "C" REX_FUNC(sub_827333E0) { WithLayoutSize<__imp__sub_827333E0, 1>(ctx, base); }
extern "C" REX_FUNC(sub_827334A0) { WithLayoutSize<__imp__sub_827334A0, 2>(ctx, base); }
extern "C" REX_FUNC(sub_82732C60) { WithLayoutSize<__imp__sub_82732C60, 4>(ctx, base); }
extern "C" REX_FUNC(sub_82732C70) { WithLayoutSize<__imp__sub_82732C70, 4>(ctx, base); }
// tw2de (2026-11-11): LR's own GUI pass (no FF XIII-2 counterpart) reads +32/+36 directly (no getter) for its
// scale (the size divides its constants) before drawing through sub_827E0630: with +36/+40 the scene's size the
// title's menu text was gone.
REX_EXTERN(__imp__sub_82739610);
extern "C" REX_FUNC(sub_82739610) { WithLayoutSize<__imp__sub_82739610, 8>(ctx, base); }
// tw2de (2026-11-11): the battle's markers over the 3D scene (the enemy's name and HP bar): sub_82457120 projects
// a position to the screen with +32/+36 read directly (sub_82457290 and sub_82458698, its callers, read them too);
// with the scene's size there the name sat at 0.8 of its place in the 1280x720 layout.
REX_EXTERN(__imp__sub_82457120);
REX_EXTERN(__imp__sub_82457290);
REX_EXTERN(__imp__sub_82458698);
extern "C" REX_FUNC(sub_82457120) { WithLayoutSize<__imp__sub_82457120, 32>(ctx, base); }
extern "C" REX_FUNC(sub_82457290) { WithLayoutSize<__imp__sub_82457290, 32>(ctx, base); }
extern "C" REX_FUNC(sub_82458698) { WithLayoutSize<__imp__sub_82458698, 32>(ctx, base); }
// tw2de (2026-11-11): the battle's enemy name and HP bar over the 3D scene (they sat at 0.8 of their place):
// sub_825547E8 reads +32/+36 with the renderer pointer kept in a register. Console, the name's x: layout_hooks 511 and
// 319 right (772), 63 / 127 / 191 wrong (592). The other such readers (8245AE10, 82402670, 824A3D80, 82554D98,
// 82564420) are not needed, not hooked.
REX_EXTERN(__imp__sub_825547E8);
extern "C" REX_FUNC(sub_825547E8) { WithLayoutSize<__imp__sub_825547E8, 256>(ctx, base); }

// The pre-made scene targets: sub_82AFD2F8(list, w, h, format, multisample) (XIII-2's sub_82AFD338) finds an unused
// one; at another scene size than 1280x720 a lookup asks the display-size chain and finds none (XIII-2 called through
// null at start-up). A lookup that finds none takes the unused target of that format and multisample whose size is
// the one asked in the other size (renderer +44/+48 against +32/+36), as in FNXF XIII-2.
REX_EXTERN(__imp__sub_82AFD2F8);
extern "C" REX_FUNC(sub_82AFD2F8) {
  const uint32_t list = ctx.r4.u32, w = ctx.r5.u32, h = ctx.r6.u32, fmt = ctx.r7.u32, ms = ctx.r8.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82AFD2F8(ctx, base);
  if (ctx.r3.u32 || !list || tw2de::RenderHeight() == 720)
    return;
  const uint32_t r = LoadParam(base, kRendererObject);
  if (!r)
    return;
  const uint32_t ow = LoadParam(base, r + 32), oh = LoadParam(base, r + 36);
  const uint32_t sw = LoadParam(base, r + 44), sh = LoadParam(base, r + 48);
  const uint32_t b = LoadParam(base, list), e = LoadParam(base, list + 4);
  auto ms_of = [&](uint32_t obj) {
    const uint32_t f = LoadParam(base, obj + 8) & 0x180;
    return f == 0 ? 0u : f == 0x80 ? 1u : f == 0x100 ? 2u : 3u;
  };
  auto near = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= 2; };
  uint32_t found = 0, at_found = 0;
  for (int pass = 0; pass < 2 && !found && ow && oh && sw && sh; ++pass) {
    const uint64_t ew = pass == 0 ? uint64_t(w) * sw / ow : uint64_t(w) * ow / sw;
    const uint64_t eh = pass == 0 ? uint64_t(h) * sh / oh : uint64_t(h) * oh / sh;
    for (uint32_t at = b; at && at < e && at - b < 8 * 256; at += 8) {
      const uint32_t obj = LoadParam(base, at);
      if (!obj || base[at + 4] || LoadParam(base, obj + 32) != fmt || ms_of(obj) != ms)
        continue;
      if (near(LoadParam(base, obj + 16), uint32_t(ew)) && near(LoadParam(base, obj + 20), uint32_t(eh))) {
        found = obj;
        at_found = at;
        break;
      }
    }
  }
  static uint32_t logged = 0;
  if (found) {
    base[at_found + 4] = 1;
    ctx.r3.u64 = found;
    if (logged++ < 24)
      REXLOG_INFO("[targets] {}x{} format {} ms {} (from {:08X}): the pre-made {}x{} one", w, h, fmt, ms, lr,
                  LoadParam(base, found + 16), LoadParam(base, found + 20));
  } else if (logged++ < 24) {
    REXLOG_INFO("[targets] none for {}x{} format {} ms {} (from {:08X}); renderer +20 {}x{} +32 {}x{} +44 {}x{}", w, h,
                fmt, ms, lr, LoadParam(base, r + 20), LoadParam(base, r + 24), ow, oh, sw, sh);
  }
}

// tw2de (2026-11-11): the GUI's clipping rectangles. With the scene's size at +32/+36 the GUI draws into a
// target of the scene's size (its viewport the target's, the 1280x720 layout projected onto it), but its clip
// rectangles come in the layout's units (the getters above give the GUI the display's size): the title's menu text,
// clipped to its row at 1280x720 coordinates, was outside its rectangle on a 1024x576 target and was not drawn. The
// engine's scissor sub_82668D08(x, y, w, h), when called from the GUI (these functions), gets them in the target's
// units (scaled by +32 / +20, +36 / +24). Its full-screen and the scene's calls are left as they are.
REX_EXTERN(__imp__sub_82668D08);
extern "C" REX_FUNC(sub_82668D08) {
  static constexpr uint32_t kGuiCallers[][2] = {{0x82733BF0u, 0x82733C78u},
                                                {0x82769050u, 0x827691F0u},
                                                {0x827ED2D8u, 0x827ED458u},
                                                {0x82849F38u, 0x8284A148u},
                                                {0x828513E8u, 0x82851AC0u},
                                                // tw2de (2026-11-11): the GUI's clamped scissor (its clamp
                                                // 0-4096; called from the GUI's 8286F5D0 / 82871D80): at 1600x900 the
                                                // menus' lines were cut at 1280 target pixels.
                                                {0x82664268u, 0x82664388u}};
  static const uint32_t mask = uint32_t(REXCVAR_GET(tw2de_layout_hooks));
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t r = tw2de::RenderHeight() != 720 && (mask & 16) ? LoadParam(base, kRendererObject) : 0;
  bool gui = false;
  for (const auto &c : kGuiCallers)
    gui |= lr >= c[0] && lr < c[1];
  if (r && gui) {
    const uint32_t dw = LoadParam(base, r + 20), dh = LoadParam(base, r + 24);
    const uint32_t tw = LoadParam(base, r + 32), th = LoadParam(base, r + 36);
    if (dw && dh && tw && th && (dw != tw || dh != th)) {
      auto scale = [](int32_t v, uint32_t to, uint32_t from) {
        return int32_t((int64_t(v) * int64_t(to) + int64_t(from) / 2) / int64_t(from));
      };
      const int32_t x = int32_t(ctx.r3.u32), y = int32_t(ctx.r4.u32), w = int32_t(ctx.r5.u32), h = int32_t(ctx.r6.u32);
      const int32_t x0 = scale(x, tw, dw), y0 = scale(y, th, dh);
      ctx.r3.u64 = uint64_t(int64_t(x0));
      ctx.r4.u64 = uint64_t(int64_t(y0));
      ctx.r5.u64 = uint64_t(int64_t(scale(x + w, tw, dw) - x0));
      ctx.r6.u64 = uint64_t(int64_t(scale(y + h, th, dh) - y0));
    }
  }
  __imp__sub_82668D08(ctx, base);
}


// tw2de: an Xbox 360 game on the ReXGlue SDK, with reNut's Direct3D-level renderer
// (renut_engine/gpu, from tw2de / reNut by masterspike52) as the alternative to the SDK's emulated Xenos GPU.
//
//   tw2de_renderer = "native"  the SDK's GPU emulation (rexgpu-xenos: D3D12 / Vulkan on the PC) - the reference
//   tw2de_renderer = "renut"   Renut's renderer through plume (Vulkan on the PC, deko3d on the Switch)
#pragma once

#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/rex_app.h>
#include <rex/ui/window.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>

#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-26): sdk/src/system/xobject.cpp
REXCVAR_DECLARE(std::string, content_alternate_roots);  // tw2de (2026-11-10): sdk/src/system/xam/content_manager.cpp
#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchStackWatermarks(bool on);  // tw2de (2026-10-26): sdk/src/core/threading_posix.cpp
#endif

#include "cpu/tw2de_enhancements.h"
#include "tw2de_romfs.h"  // tw2de (2026-11-09): Preload
#include "cpu/tw2de_glyphs.h"  // tw2de (2026-10-25): GlyphsInit
#include "cpu/tw2de_handheld_gpu.h"  // tw2de (2026-10-25): HandheldGpuInit
#include "cpu/tw2de_texts.h"  // tw2de (2026-10-25): TextsInit
#include "cpu/tw2de_images.h"  // tw2de (2026-10-25): ImagesInit
#include "cpu/tw2de_layouts.h"  // tw2de (2026-10-25): LayoutsInit
#include "cpu/tw2de_switch_buttons.h"  // tw2de (2026-10-25): SwitchButtonsInit
#include "cpu/tw2de_switch_ui.h"  // TW2 (2026-11-14): SwitchUiInit
#include "cpu/tw2de_texture_sources.h"
#include "profiler/profiler.h"
#include "pgo_switch.h"
#if TW2DE_RENUT_RENDERER
#include "renut_engine/gpu/tw2de_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#if REX_PLATFORM_SWITCH
#include "renut_engine/gpu/nx_shaders/shader_prepass_switch.h"
#include "renut_engine/gpu/nx_shaders/boot_menu_switch.h"
#include "renut_engine/gpu/nx_shaders/thread_alloc.h"  // tw2de (2026-10-26): HeapSitesEnable
#endif
#endif

class FnxfXiiiLrApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<FnxfXiiiLrApp>(new FnxfXiiiLrApp(ctx, "tw2de", PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    tw2de::romfs::Preload();  // tw2de (2026-11-09): the NRO's romfs into RAM, the NRO file closed
#if REX_PLATFORM_SWITCH && TW2DE_RENUT_RENDERER
    // tw2de (2026-10-26): the memory report's heap sites (renut::nxs::HeapSitesReport), from here on: before the
    // shader store, the DKSH cache and the rest of the start-up's big blocks.
    if (REXCVAR_GET(log_diagnostics)) {
      renut::nxs::HeapSitesEnable();
      RexSwitchStackWatermarks(true);  // tw2de (2026-10-26): the report's host stack depths
    }
#endif
#if TW2DE_PGO_GENERATE
    tw2de::StartPgoDumper();
#endif
    tw2de::ApplyMasterSwitches();
    tw2de::HandheldGpuInit();  // tw2de (2026-10-25): tw2de_handheld_gpu_460
#if TW2DE_RENUT_RENDERER
    if (tw2de::renut_gpu::Active()) {
      // No SDK graphics system and no GPU plugin: the renderer is the app's.
      renut::gpu::ConfigureRuntime(config);
#if REX_PLATFORM_SWITCH
      // tw2de (2026-10-25): the boot menu (language, frame interval, resolution, anti-aliasing, upscaler): at the
      // first start (no tw2de_settings.ini) or with the left stick held pressed in; its settings applied at every
      // start. Before the shader pre-pass: when the shaders have to be compiled again (a new build), the menu does
      // not wait for it (the pre-pass reads none of the menu's settings).
      renut::nxs::RunBootMenu(user_data_root());  // tw2de (2026-10-28): the achievements
      // tw2de (2026-10-21): the game's shaders, found in its files, translated and compiled on the console before
      // the window exists (the pre-pass draws on it itself); at once when they all are already.
      renut::nxs::RunSwitchShaderPrepass("sdmc:/switch/tw2de_nx/assets");
#if TW2DE_PGO_GENERATE
      // tw2de (2026-10-26): the counts up to here (the pre-pass, when it compiled) as a part of their own.
      tw2de::PgoDump("prepass");
#endif
#endif
    }
#endif
    // tw2de (2026-10-25): the language pack (tw2de_lang, which the boot menu sets: so after it): its characters,
    // then its texts (written with them), its pictures and layouts, then the file reads watched (for its changes to
    // the game's files: tw2de_wpd_patch.cpp; and tw2de_diag_texture_sources).
    tw2de::GlyphsInit();
    tw2de::TextsInit();
    tw2de::ImagesInit();
    tw2de::LayoutsInit();
    tw2de::SwitchButtonsInit();  // tw2de (2026-10-25): the controller picture
    tw2de::TextureSourcesInit();
    tw2de::SwitchUiInit();  // TW2 (2026-11-14): the patched view of pack0.dzip, before the game's files are mounted
  }

#if TW2DE_RENUT_RENDERER
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
    if (tw2de::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }
#endif

  void OnPostSetup() override {
    tw2de::LogEnhancements();
    tw2de::profiler::Start();  // tw2de (2026-10-24): the Switch CPU profiler, when tw2de_cpu_profiler asks for it
  }

  void OnShutdown() override {
#if TW2DE_PGO_GENERATE
    tw2de::PgoDump();
#endif
#if TW2DE_RENUT_RENDERER
    if (tw2de::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
#endif
  }

  // The game's files. Switch: always sdmc:/switch/tw2de_nx/assets. PC: game_data_root from tw2de.toml, else an
  // "assets" folder next to the executable.
  void OnConfigurePaths(rex::PathConfig& paths) override {
#if REX_PLATFORM_SWITCH
    // The rooted form: libstdc++ takes "sdmc:/..." for a relative path and std::filesystem::absolute put the current
    // directory in front of it (Runtime::SetupVfs threw: "sdmc:/switch/tw2de_nx/sdmc:/switch/tw2de_nx/assets").
    // newlib resolves "/switch/..." on the SD card.
    paths.game_data_root = "/switch/tw2de_nx/assets";
    // tw2de (2026-11-10): the title update's data files (filelist_patch_e / _scr_e, white_patch_e / _scr_e) sit
    // next to default.xex, and LR opens them through UPDATE:\ (with no update: device the patch's files were never
    // read). An update_data_root in tw2de.toml still wins.
    if (paths.update_data_root.empty())
      paths.update_data_root = paths.game_data_root;
    // tw2de (2026-10-23): the user data (saves, achievements, the runtime's cache) in a "User" folder next to the
    // NRO, not in "tw2de" (the SDK's default: the app's name under the user folder, which on the Switch is the NRO's
    // folder). A user_data_root / cache_root in tw2de.toml still wins (rex_app.cpp applies it after this).
    paths.user_data_root = "/switch/tw2de_nx/User";
    paths.cache_root = "/switch/tw2de_nx/User/cache";
    // tw2de (2026-11-10): the saves of FF XIII (535107E4) and FF XIII-2 (53510806), this game's XEX alternate
    // title IDs, from FNXF XIII's and FNXF XIII-2's folders, unless tw2de.toml names others (FF XIII-2 reads FF
    // XIII's the same way: the XDK's cross-title enumeration, XamContentAggregateCreateEnumerator). Not tested in LR yet.
    if (REXCVAR_GET(content_alternate_roots).empty())
      REXCVAR_SET(content_alternate_roots,
                  std::string("535107E4=/switch/fnxf_xiii/User;53510806=/switch/fnxf_xiii-2/User"));
#else
    if (!paths.game_data_root.empty()) return;
    const auto folder = rex::filesystem::GetExecutableFolder();
    std::error_code ec;
    if (!folder.empty() && std::filesystem::is_directory(folder / "assets", ec))
      paths.game_data_root = folder / "assets";
    if (paths.update_data_root.empty())
      paths.update_data_root = paths.game_data_root;  // the title update's data files (see the Switch branch)
#endif
  }

  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
#if TW2DE_RENUT_RENDERER
    // The window exists here: Renut's renderer comes up before the game starts.
    if (tw2de::renut_gpu::Active())
      renut::gpu::StartRenderer();
#endif
    return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }
};

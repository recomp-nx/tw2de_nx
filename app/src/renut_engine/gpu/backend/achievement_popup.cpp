// tw2de (2026-10-23): achievement_popup.h.
#include "renut_engine/gpu/backend/achievement_popup.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <plume_render_interface.h>

#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/popup_compose.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/log.h"

#if defined(PLUME_DEKO3D) && defined(__SWITCH__)
#define RENUT_ACHIEVEMENT_POPUP 1
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <switch.h>
#include <rex/runtime.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>
#include <toml++/toml.hpp>
#include "cpu/tw2de_lang_pack.h"  // tw2de (2026-10-28): a language pack's header and achievement names
#include "renut_engine/gpu/nx_shaders/ui_language.h"  // TW2 (2026-11-14)
#include "renut_engine/gpu/nx_shaders/prepass_screen.h"  // tw2de (2026-10-28): RasterGlyphs
#include "host_shaders/popup_ps.deko3d.h"
#else
#define RENUT_ACHIEVEMENT_POPUP 0
#endif

REXCVAR_DECLARE(bool, tw2de_achievement_popup);
REXCVAR_DECLARE(double, tw2de_achievement_popup_seconds);
REXCVAR_DECLARE(bool, tw2de_achievement_popup_test);
REXCVAR_DECLARE(uint32_t, user_language);

namespace renut::gpu::achievement_popup {

#if RENUT_ACHIEVEMENT_POPUP
namespace {

constexpr int64_t kFadeInMs = 250, kFadeOutMs = 600;
constexpr int32_t kMarginPx = 16;  // from the picture's top left, at 720 lines
constexpr int64_t kTestDelayMs = 4000;
constexpr plume::RenderFormat kFormat = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;  // video.cpp's

struct Pending {
  uint32_t id = 0;
  std::string name;
  uint32_t gamerscore = 0;
};

std::mutex g_queue_mutex;
std::deque<Pending> g_queue;  // filled by the achievement manager's callback (any thread)

struct Popup {
  bool ready = false;
  std::unique_ptr<plume::RenderShader> ps;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderCommandList> list;
  std::unique_ptr<plume::RenderCommandFence> fence;
  uint32_t slot = kInvalidDescriptor;
  uint32_t width = 0, height = 0;
  bool showing = false;
  int64_t shown_at = 0;
  bool registered = false;
  rex::system::AchievementListenerHandle listener = 0;
  int64_t started_at = 0;
  bool test_done = false;
};
Popup g;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Enqueue(uint32_t id, std::string name, uint32_t gamerscore) {
  std::lock_guard lock(g_queue_mutex);
  g_queue.push_back({id, std::move(name), gamerscore});
}

// tw2de (2026-10-28): a language pack's achievements.toml (the form rexglue init achievements writes:
// [[achievements]] with id and label; the other keys are not used here), read once: the pack's name for an
// achievement, or 'fallback' (no pack, no file, or no such id).
std::string PackLabel(uint32_t id, const std::string &fallback) {
  static const std::map<uint32_t, std::string> labels = [] {
    std::map<uint32_t, std::string> out;
    const tw2de::LangPack *pack = tw2de::GetLangPack();
    const std::vector<uint8_t> *file = pack ? pack->Find("achievements.toml") : nullptr;
    if (!file)
      return out;
    try {
      const toml::table table =
          toml::parse(std::string_view(reinterpret_cast<const char *>(file->data()), file->size()));
      if (const toml::array *list = table["achievements"].as_array()) {
        for (const toml::node &node : *list) {
          const toml::table *entry = node.as_table();
          if (!entry)
            continue;
          const int64_t key = (*entry)["id"].value_or<int64_t>(0);
          std::string label = (*entry)["label"].value_or<std::string>("");
          if (key > 0 && !label.empty())
            out[uint32_t(key)] = std::move(label);
        }
      }
      RGPU_INFO("[achievement popup] {} names from the language pack's achievements.toml", out.size());
    } catch (const toml::parse_error &e) {
      RGPU_WARN("[achievement popup] the language pack's achievements.toml: {}", e.what());
    }
    return out;
  }();
  auto it = labels.find(id);
  return it == labels.end() ? fallback : it->second;
}

// tw2de (2026-10-28): the popup in the system's font (the boot menu's and the pre-pass screen's), rasterised for
// the letters it shows, so any language's letters are there; an empty image when the shared font cannot be had (the
// caller then takes the baked font).
popup::Image ComposeInSystemFont(const std::string &header, const std::string &name, uint32_t gamerscore, bool large) {
  popup::Image img;
  const bool pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
  PlFontData font{};
  if (pl && R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) &&
      (renut::nxs::UseSystemFallbackFonts(), true)) {
    // The baked fonts' sizes: ascent + descent 21 pixels at 720 lines, 32 at 1080.
    std::vector<renut::nxs::GlyphBitmap> glyphs;
    popup::OwnedFont owned;
    if (renut::nxs::RasterGlyphs(static_cast<const uint8_t *>(font.address), font.size, large ? 32.0f : 21.0f,
                                 popup::CodePointsOf(header, popup::SecondLine(name, gamerscore)), owned.ascent,
                                 owned.descent, glyphs)) {
      for (const auto &g : glyphs)
        owned.Add(g.codepoint, g.advance, g.x, g.y, g.w, g.h, g.coverage.data());
      img = popup::Compose(header, name, gamerscore, large, owned);
    }
  }
  if (pl)
    plExit();
  return img;
}

// tw2de (2026-10-28): the boot menu's achievement lists (nx_shaders/boot_menu_switch.cpp) run before the game is
// loaded, so the names come from here: once the game's achievement data (default.xex's XDBF) is there, every
// achievement's gamerscore and its name and descriptions in each of the game's languages are written to
// <user data>/achievements/names.toml (only when they differ from the file's). From the player's own copy of the game.
void WriteNamesLocked() {
  static bool done = false;
  if (done || !g.registered)
    return;
  rex::system::KernelState *ks = rex::system::kernel_state();
  if (!ks || !ks->emulator())
    return;
  const rex::system::util::XdbfGameData db = ks->title_xdbf();
  if (!db.is_valid())
    return;  // not loaded yet: tried again at the next present
  done = true;
  const auto entries = db.GetAchievements();
  if (entries.empty())
    return;
  struct Lang {
    rex::system::XLanguage id;
    const char *key;  // the boot menu's language keys
  };
  static constexpr Lang kLangs[] = {{rex::system::XLanguage::kEnglish, "english"},
                                    {rex::system::XLanguage::kGerman, "german"},
                                    {rex::system::XLanguage::kFrench, "french"},
                                    {rex::system::XLanguage::kSpanish, "spanish"},
                                    {rex::system::XLanguage::kItalian, "italian"}};
  toml::table root;
  root.insert("title_id", fmt::format("{:08X}", ks->title_id()));
  toml::array list;
  for (const auto &e : entries) {
    toml::table t;
    t.insert("id", int64_t(uint16_t(e.id)));
    t.insert("gamerscore", int64_t(uint16_t(e.gamerscore)));
    t.insert("flags", int64_t(uint32_t(e.flags)));
    for (const Lang &l : kLangs) {
      if (db.title(l.id).empty())
        continue;  // not one of the game's languages
      t.insert(std::string("label_") + l.key, db.GetStringTableEntry(l.id, uint16_t(e.label_id)));
      t.insert(std::string("description_") + l.key, db.GetStringTableEntry(l.id, uint16_t(e.description_id)));
      t.insert(std::string("locked_") + l.key, db.GetStringTableEntry(l.id, uint16_t(e.unachieved_id)));
    }
    list.push_back(std::move(t));
  }
  root.insert("achievements", std::move(list));
  std::ostringstream text;
  text << "# The game's achievements, for the boot menu (tw2de writes this from default.xex at each start).\n\n"
       << root << "\n";
  const std::filesystem::path path = ks->emulator()->user_data_root() / "achievements" / "names.toml";
  {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream old;
    if (in)
      old << in.rdbuf();
    if (in && old.str() == text.str())
      return;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << text.str();
    if (!out.good()) {
      RGPU_WARN("[achievement popup] could not write {}", tmp.string());
      return;
    }
  }
  std::filesystem::remove(path, ec);
  std::filesystem::rename(tmp, path, ec);
  RGPU_INFO("[achievement popup] {} achievements written to {} for the boot menu{}", entries.size(), path.string(),
            ec ? " (rename failed: " + ec.message() + ")" : "");
}

// The achievement manager exists once the kernel does, which may be after the renderer starts: tried at each present
// until it is there.
void RegisterLocked() {
  if (g.registered)
    return;
  rex::system::KernelState *ks = rex::system::kernel_state();
  if (!ks)
    return;
  g.listener = ks->achievements().RegisterNotificationCallback([](const rex::system::AchievementEvent &e) {
    if (REXCVAR_GET(tw2de_achievement_popup))
      Enqueue(e.achievement.id, e.achievement.label, e.achievement.gamerscore);
  });
  g.registered = true;
  if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("[achievement popup] listening for unlocks");
}

// The test popup: the game's first achievement (by id), or a sample with every kind of letter the font covers.
void QueueTestLocked() {
  g.test_done = true;
  std::string name;
  uint32_t id = 0;
  uint32_t gamerscore = 0;
  if (rex::system::KernelState *ks = rex::system::kernel_state()) {
    auto list = ks->achievements().ListAchievements();
    std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    if (!list.empty()) {
      id = list.front().id;
      name = list.front().label;
      gamerscore = list.front().gamerscore;
    }
  }
  if (name.empty())
    name = "\xC3\x84\xC3\x96\xC3\x9C\xC3\x9F \xC3\x80\xC3\x87\xC3\x89\xC3\x88 \xC3\xA0\xC3\xA7\xC3\xA9\xC3\xA8\xC3\xAA "
           "\xC5\x93 \xC3\xB1\xC2\xBF\xC2\xA1 \xC3\xAC\xC3\xB2\xC3\xB9 \xC2\xAB\xE2\x80\xA6\xC2\xBB";  // ÄÖÜß ÀÇÉÈ àçéèê œ ñ¿¡ ìòù «…»
  Enqueue(id, name, gamerscore);
}

// Makes the next queued popup's image and uploads it (its own list, waited for: once a popup, not a frame).
bool StartNextLocked(VideoState &s, uint32_t out_h) {
  Pending next;
  {
    std::lock_guard lock(g_queue_mutex);
    if (g_queue.empty())
      return false;
    next = std::move(g_queue.front());
    g_queue.pop_front();
  }
  const bool large = out_h >= 900;
  // tw2de (2026-10-28): the header and name a language pack gives (its [port] achievement_unlocked, its
  // achievements.toml), in the system's font; the baked font if that cannot be had.
  // TW2 (2026-11-14): Polish, Czech, Hungarian, Russian and Japanese by the game's language (ui_language.h: the
  // Xbox language cannot tell Czech and Hungarian from English).
  static constexpr const char *kHeaderMore[] = {"Osiągnięcie odblokowane", "Úspěch odemčen", "Teljesítmény feloldva", "Достижение получено", "実績を解除しました", "成就已解锁",
                                               "Başarı kilidi açıldı"};  // TW2 (2026-11-15): Turkish
  const int ui_language = renut::nxs::UiLanguage();
  const std::string header_default =
      ui_language >= 5 && ui_language < 5 + int(sizeof(kHeaderMore) / sizeof(kHeaderMore[0]))
          ? std::string(kHeaderMore[ui_language - 5])
          : std::string(popup::HeaderText(REXCVAR_GET(user_language)));
  const std::string header = tw2de::PortText("achievement_unlocked", header_default.c_str());
  const std::string name = PackLabel(next.id, next.name);
  popup::Image img = ComposeInSystemFont(header, name, next.gamerscore, large);
  if (!img.width || !img.height)
    img = popup::Compose(header, name, next.gamerscore, large);
  if (!img.width || !img.height)
    return false;

  auto texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(img.width, img.height, 1, kFormat));
  auto view = texture ? texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kFormat)) : nullptr;
  const uint32_t row_texels = (img.width + 63) & ~63u;  // 256-byte rows
  auto staging = s.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(uint64_t(row_texels) * img.height * 4));
  auto *texels = staging ? static_cast<uint8_t *>(staging->map()) : nullptr;
  if (!view || !texels)
    return false;
  for (uint32_t y = 0; y < img.height; ++y)
    std::memcpy(texels + size_t(y) * row_texels * 4, img.rgba.data() + size_t(y) * img.width * 4, size_t(img.width) * 4);
  staging->unmap();
  plume::RenderCommandList *cmd = g.list.get();
  cmd->begin();
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture.get(), 0, 0),
                         plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), kFormat, img.width,
                                                                           img.height, 1, row_texels, 0),
                         0, 0, 0, nullptr);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::SHADER_READ));
  cmd->end();
  const plume::RenderCommandList *lists[] = {cmd};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, g.fence.get());
  s.queue->waitForCommandFence(g.fence.get());
  // The queue runs its lists in order, so with this list done every frame submitted before it is too: none still reads
  // the previous popup's texture, which goes now. The slot names the new one for the frames recorded from here on.
  Video::SetTextureSlotLocked(g.slot, texture.get(), view.get());
  g.texture = std::move(texture);
  g.view = std::move(view);
  g.width = img.width;
  g.height = img.height;
  g.showing = true;
  g.shown_at = NowMs();
  RGPU_INFO("[achievement popup] \"{}\" ({}G), {}x{}", next.name, next.gamerscore, img.width, img.height);
  return true;
}

}  // namespace

void InitLocked(VideoState &s) {
  if (g.ready || !s.device || !s.queue || !s.present_vs || !s.utility_layout)
    return;
  g.ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(popup_ps), "popup_ps");
  if (!g.ps)
    return;
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.utility_layout.get();
  desc.vertexShader = s.present_vs.get();
  desc.pixelShader = g.ps.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = kSwapChainFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  g.pipeline = s.device->createGraphicsPipeline(desc);
  g.list = s.queue->createCommandList();
  g.fence = s.device->createCommandFence();
  if (!g.pipeline || !g.list || !g.fence) {
    ShutdownLocked(s);
    return;
  }
  g.slot = Video::AllocateTextureSlotLocked();
  if (g.slot == kInvalidDescriptor) {
    ShutdownLocked(s);
    return;
  }
  g.started_at = NowMs();
  g.ready = true;
  if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("[achievement popup] ready");
}

void ShutdownLocked(VideoState &s) {
  (void)s;
  if (g.registered) {
    if (rex::system::KernelState *ks = rex::system::kernel_state())
      ks->achievements().UnregisterCallback(g.listener);
    g.registered = false;
  }
  g.ready = false;
  g.showing = false;
  if (g.slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(g.slot);
  g.slot = kInvalidDescriptor;
  g.list.reset();
  g.fence.reset();
  g.view.reset();
  g.texture.reset();
  g.pipeline.reset();
  g.ps.reset();
}

bool DrawLocked(VideoState &s, plume::RenderCommandList *cmd, uint32_t out_w, uint32_t out_h, int32_t x0, int32_t y0,
                int32_t top) {
  if (!g.ready)
    return false;
  RegisterLocked();
  WriteNamesLocked();  // tw2de (2026-10-28): once, for the boot menu
  const int64_t now = NowMs();
  if (!g.test_done && REXCVAR_GET(tw2de_achievement_popup_test) && now - g.started_at >= kTestDelayMs)
    QueueTestLocked();
  const int64_t hold = int64_t(REXCVAR_GET(tw2de_achievement_popup_seconds) * 1000.0);
  if (g.showing && now - g.shown_at >= hold + kFadeOutMs)
    g.showing = false;
  if (!g.showing && !StartNextLocked(s, out_h))
    return false;
  if (!REXCVAR_GET(tw2de_achievement_popup))
    return false;
  const int64_t t = now - g.shown_at;
  float opacity = 1.0f;
  if (t < kFadeInMs)
    opacity = float(t) / float(kFadeInMs);
  else if (t > hold)
    opacity = 1.0f - float(t - hold) / float(kFadeOutMs);
  opacity = std::clamp(opacity, 0.0f, 1.0f);

  const float k = float(out_h) / 720.0f;
  const int32_t margin = int32_t(std::lround(kMarginPx * k));
  const int32_t x = std::max(x0, 0) + margin;
  const int32_t y = std::max(std::max(y0, 0) + margin, top + margin / 2);
  if (x + int32_t(g.width) > int32_t(out_w) || y + int32_t(g.height) > int32_t(out_h))
    return false;
  cmd->setViewports(plume::RenderViewport(float(x), float(y), float(g.width), float(g.height)));
  cmd->setScissors(plume::RenderRect(x, y, x + int32_t(g.width), y + int32_t(g.height)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(g.pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.translate[0] = -float(x);
  push.translate[1] = -float(y);
  push.texture_index = g.slot;
  push.sampler_index = kLinearClampSamplerSlot;
  push.param0 = opacity;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  return true;
}

#else

void InitLocked(VideoState &) {}
void ShutdownLocked(VideoState &) {}
bool DrawLocked(VideoState &, plume::RenderCommandList *, uint32_t, uint32_t, int32_t, int32_t, int32_t) {
  return false;
}

#endif

}  // namespace renut::gpu::achievement_popup

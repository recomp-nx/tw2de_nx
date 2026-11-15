// tw2de (2026-10-21): the shader pre-pass on the Switch, before the game starts (FnxfXiiiLrApp::OnPreSetup): the
// screen (prepass_screen.h) through libnx's framebuffer on the default window, which is free until the renderer makes
// its swapchain; the work (shader_prepass.h) on three threads pinned to cores 0-2 with the CPU boosted; uam compiling
// into the deko3d back end's DKSH cache.
#include "renut_engine/gpu/nx_shaders/shader_prepass_switch.h"
#include "renut_engine/gpu/nx_shaders/front_end_store.h"
#include "renut_engine/gpu/nx_shaders/late_spec.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <malloc.h>  // tw2de (2026-11-08): mallinfo

#include <switch.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "renut_engine/gpu/tw2de_renut_deko3d.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"
#include "renut_engine/gpu/nx_shaders/prepass_screen.h"
#include "renut_engine/gpu/nx_shaders/boot_menu_switch.h"  // tw2de (2026-11-07): ScreenWants1080p, ScreenFramebuffer
#include "renut_engine/gpu/nx_shaders/shader_prepass.h"
#include "renut_engine/gpu/nx_shaders/shader_store.h"
#include "renut_engine/gpu/nx_shaders/thread_alloc.h"
#include "renut_engine/gpu/nx_shaders/thread_waits.h"
#include "renut_engine/gpu/nx_shaders/ui_language.h"
#include "renut_engine/gpu/nx_shaders/early_exit.h"
#include "cpu/tw2de_lang_pack.h"  // tw2de (2026-10-25): a language pack's text
#include "renut_engine/gpu/nx_shaders/shader_store.h"
#include "uam_api.h"
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(tw2de_shader_prepass, true, "tw2de",
                    "Switch: before the game starts, find the shaders in the game files, translate them and compile "
                    "them with uam (once; resumable)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, tw2de_gpu_bool_spec);
REXCVAR_DECLARE(int32_t, tw2de_fsr3);  // render/fsr3.cpp (TW2, 2026-11-15: the motion-vector twins)
REXCVAR_DEFINE_BOOL(tw2de_prepass_compile, true, "tw2de",
                    "Switch: the pre-pass compiles the shaders with uam (false: it only finds and translates them; the "
                    "game then compiles each one when it first draws with it: a diagnostic, for a fresh DKSH cache)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_prepass_retry_failed, false, "tw2de",
                    "Switch: a diagnostic: the pre-pass compiles again the variants it recorded as failed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_diag_prepass_recompile, false, "Game/Diagnostics",
                    "Test (2026-11-08): the pre-pass compiles every shader and variant again, as after an update that "
                    "changes the translation, and keeps none of it (the DKSH cache stays as it is): its memory over a "
                    "whole run")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_prepass_threads, 3, "tw2de",
                     "Switch: threads of the shader pre-pass, spread over cores 0-2 (3: one a core)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" u32 __nx_applet_exit_mode;  // libnx: 0 returns to the loader at exit
extern "C" NX_NORETURN void __libnx_exit(int rc);


REXCVAR_DECLARE(bool, tw2de_deko3d_late_spec);

namespace renut::nxs {

namespace {

// "m:ss", or "h:mm:ss" from an hour.
std::string Clock(double seconds) {
  const long t = long(seconds);
  return t >= 3600 ? fmt::format("{}:{:02}:{:02}", t / 3600, (t / 60) % 60, t % 60)
                   : fmt::format("{}:{:02}", t / 60, t % 60);
}

// tw2de (2026-10-25): the pre-pass screen's text in the game's language (ui_language.h: the boot menu's
// language, set before the pre-pass). Format strings for fmt (runtime): "{}" are numbers or times.
struct PrepassStrings {
  const char *thousands;  // the thousands separator
  bool decimal_comma;
  const char *title, *footer;
  const char *step_cached, *step_scan, *step_recompile, *step_compile, *step_front_end, *step_index, *step_check;
  const char *n_of_total, *scan_detail, *failed, *compiled;
  const char *eta_short, *eta_minutes, *eta_hours;
  const char *this_step, *total, *step1_took;
};
constexpr PrepassStrings kPrepassStrings[kUiLanguageCount] = {
    {   // English
        ",", false, "Preparing shaders",
        "Done once, and kept on the SD card. The game starts when this is finished.\n"
        "You can close the game at any time: the next start continues where this one stopped.",
        "Step 1 of 4 · Optimizing cached shaders", "Step 1 of 4 · Finding the shaders in the game files",
        "Step 2 of 4 · Recompiling cached shaders", "Step 2 of 4 · Compiling the shaders",
        "Step 3 of 4 · Generating intermediates", "Step 4 of 4 · Creating sorted index", "Checking the shaders...",
        "{} of {} shaders", "{} of {} MB · {} shaders found, {} new", " · {} failed", "{} shaders compiled",
        "less than 2 minutes left", "about {} minutes left", "about {} hours left",
        "This step: {} so far", " · Total: {}", " (step 1 took {})",
    },
    {   // Deutsch
        ".", true, "Shader werden vorbereitet",
        "Geschieht nur einmal und wird auf der SD-Karte gespeichert. Das Spiel startet, sobald dies fertig ist.\n"
        "Das Spiel kann jederzeit geschlossen werden: Der nächste Start macht dort weiter, wo dieser aufgehört hat.",
        "Schritt 1 von 4 · Gespeicherte Shader optimieren", "Schritt 1 von 4 · Shader in den Spieldateien suchen",
        "Schritt 2 von 4 · Gespeicherte Shader neu kompilieren", "Schritt 2 von 4 · Shader kompilieren",
        "Schritt 3 von 4 · Zwischenformen erzeugen", "Schritt 4 von 4 · Sortierten Index erstellen",
        "Shader werden geprüft...",
        "{} von {} Shadern", "{} von {} MB · {} Shader gefunden, {} neu", " · {} fehlgeschlagen",
        "{} Shader kompiliert",
        "noch weniger als 2 Minuten", "noch etwa {} Minuten", "noch etwa {} Stunden",
        "Dieser Schritt: bisher {}", " · Gesamt: {}", " (Schritt 1 dauerte {})",
    },
    {   // Français
        " ", true, "Préparation des shaders",
        "Effectué une seule fois et conservé sur la carte SD. Le jeu démarre une fois cette étape terminée.\n"
        "Vous pouvez fermer le jeu à tout moment : le prochain lancement reprendra là où celui-ci s'est arrêté.",
        "Étape 1 sur 4 · Optimisation des shaders en cache",
        "Étape 1 sur 4 · Recherche des shaders dans les fichiers du jeu",
        "Étape 2 sur 4 · Recompilation des shaders en cache", "Étape 2 sur 4 · Compilation des shaders",
        "Étape 3 sur 4 · Génération des formes intermédiaires", "Étape 4 sur 4 · Création de l'index trié",
        "Vérification des shaders...",
        "{} sur {} shaders", "{} sur {} Mo · {} shaders trouvés, {} nouveaux", " · {} en échec",
        "{} shaders compilés",
        "moins de 2 minutes restantes", "environ {} minutes restantes", "environ {} heures restantes",
        "Cette étape : {} écoulées", " · Total : {}", " (étape 1 : {})",
    },
    {   // Español
        ".", true, "Preparando los shaders",
        "Se hace una sola vez y se guarda en la tarjeta SD. El juego se inicia cuando termine.\n"
        "Puedes cerrar el juego en cualquier momento: el próximo inicio continuará donde se quedó este.",
        "Paso 1 de 4 · Optimizando los shaders guardados", "Paso 1 de 4 · Buscando los shaders en los archivos del juego",
        "Paso 2 de 4 · Recompilando los shaders guardados", "Paso 2 de 4 · Compilando los shaders",
        "Paso 3 de 4 · Generando formas intermedias", "Paso 4 de 4 · Creando el índice ordenado",
        "Comprobando los shaders...",
        "{} de {} shaders", "{} de {} MB · {} shaders encontrados, {} nuevos", " · {} fallidos",
        "{} shaders compilados",
        "quedan menos de 2 minutos", "quedan unos {} minutos", "quedan unas {} horas",
        "Este paso: {} hasta ahora", " · Total: {}", " (el paso 1 tardó {})",
    },
    {   // Italiano
        ".", true, "Preparazione degli shader",
        "Si fa una sola volta e viene salvato sulla scheda SD. Il gioco si avvia al termine.\n"
        "Puoi chiudere il gioco in qualsiasi momento: al prossimo avvio si riprenderà da dove si è interrotto.",
        "Passo 1 di 4 · Ottimizzazione degli shader in cache", "Passo 1 di 4 · Ricerca degli shader nei file del gioco",
        "Passo 2 di 4 · Ricompilazione degli shader in cache", "Passo 2 di 4 · Compilazione degli shader",
        "Passo 3 di 4 · Generazione delle forme intermedie", "Passo 4 di 4 · Creazione dell'indice ordinato",
        "Verifica degli shader...",
        "{} di {} shader", "{} di {} MB · {} shader trovati, {} nuovi", " · {} non riusciti", "{} shader compilati",
        "meno di 2 minuti rimanenti", "circa {} minuti rimanenti", "circa {} ore rimanenti",
        "Questo passo: {} finora", " · Totale: {}", " (passo 1: {})",
    },
    {   // Polski (TW2, 2026-11-14)
        " ", true,
        "Przygotowywanie shaderów",
        "Odbywa się tylko raz, a wynik zostaje na karcie SD. Gra uruchomi się po zakończeniu.\nGrę można zamknąć w dowolnej chwili: następne uruchomienie wznowi pracę w miejscu przerwania.",
        "Krok 1 z 4 · Optymalizacja zapisanych shaderów",
        "Krok 1 z 4 · Wyszukiwanie shaderów w plikach gry",
        "Krok 2 z 4 · Ponowna kompilacja zapisanych shaderów",
        "Krok 2 z 4 · Kompilacja shaderów",
        "Krok 3 z 4 · Tworzenie form pośrednich",
        "Krok 4 z 4 · Tworzenie posortowanego indeksu",
        "Sprawdzanie shaderów...",
        "{} z {} shaderów",
        "{} z {} MB · znaleziono {} shaderów, {} nowych",
        " · {} nieudanych",
        "Skompilowano {} shaderów",
        "zostały mniej niż 2 minuty",
        "zostało około {} min",
        "zostało około {} godz.",
        "Ten krok: dotąd {}",
        " · Łącznie: {}",
        " (krok 1 trwał {})",
    },
    {   // Čeština (TW2, 2026-11-14)
        " ", true,
        "Příprava shaderů",
        "Provádí se jen jednou a výsledek zůstane na SD kartě. Hra se spustí po dokončení.\nHru můžete kdykoli zavřít: příští spuštění bude pokračovat tam, kde toto skončilo.",
        "Krok 1 ze 4 · Optimalizace uložených shaderů",
        "Krok 1 ze 4 · Hledání shaderů v souborech hry",
        "Krok 2 ze 4 · Opětovná kompilace uložených shaderů",
        "Krok 2 ze 4 · Kompilace shaderů",
        "Krok 3 ze 4 · Vytváření mezitvarů",
        "Krok 4 ze 4 · Vytváření seřazeného indexu",
        "Kontrola shaderů...",
        "{} z {} shaderů",
        "{} z {} MB · nalezeno {} shaderů, {} nových",
        " · {} selhalo",
        "Zkompilováno {} shaderů",
        "zbývají méně než 2 minuty",
        "zbývá asi {} min",
        "zbývá asi {} h",
        "Tento krok: zatím {}",
        " · Celkem: {}",
        " (krok 1 trval {})",
    },
    {   // Magyar (TW2, 2026-11-14)
        " ", true,
        "Shaderek előkészítése",
        "Csak egyszer történik meg, az eredmény az SD-kártyán marad. A játék a befejezés után indul el.\nA játékot bármikor bezárhatod: a következő indítás ott folytatja, ahol ez abbamaradt.",
        "1. lépés (4-ből) · Mentett shaderek optimalizálása",
        "1. lépés (4-ből) · Shaderek keresése a játékfájlokban",
        "2. lépés (4-ből) · Mentett shaderek újrafordítása",
        "2. lépés (4-ből) · Shaderek fordítása",
        "3. lépés (4-ből) · Köztes formák létrehozása",
        "4. lépés (4-ből) · Rendezett index létrehozása",
        "Shaderek ellenőrzése...",
        "{} / {} shader",
        "{} / {} MB · {} shader található, {} új",
        " · {} sikertelen",
        "{} shader lefordítva",
        "kevesebb mint 2 perc van hátra",
        "kb. {} perc van hátra",
        "kb. {} óra van hátra",
        "Ez a lépés: eddig {}",
        " · Összesen: {}",
        " (az 1. lépés {} volt)",
    },
    {   // Русский (TW2, 2026-11-14)
        " ", true,
        "Подготовка шейдеров",
        "Выполняется один раз, результат сохраняется на SD-карте. Игра запустится по завершении.\nИгру можно закрыть в любой момент: следующий запуск продолжит с того места, где остановился этот.",
        "Шаг 1 из 4 · Оптимизация сохранённых шейдеров",
        "Шаг 1 из 4 · Поиск шейдеров в файлах игры",
        "Шаг 2 из 4 · Повторная компиляция сохранённых шейдеров",
        "Шаг 2 из 4 · Компиляция шейдеров",
        "Шаг 3 из 4 · Создание промежуточных форм",
        "Шаг 4 из 4 · Создание отсортированного индекса",
        "Проверка шейдеров...",
        "{} из {} шейдеров",
        "{} из {} МБ · найдено шейдеров: {}, новых: {}",
        " · с ошибкой: {}",
        "Скомпилировано шейдеров: {}",
        "осталось меньше 2 минут",
        "осталось около {} мин",
        "осталось около {} ч",
        "Этот шаг: прошло {}",
        " · Всего: {}",
        " (шаг 1 занял {})",
    },
    {   // 日本語 (TW2, 2026-11-14)
        ",", false,
        "シェーダーを準備中",
        "この処理は一度だけ行われ、結果はSDカードに保存されます。完了するとゲームが始まります。\nゲームはいつでも終了できます。次回の起動時に続きから再開します。",
        "ステップ 1 / 4 · 保存済みシェーダーの最適化",
        "ステップ 1 / 4 · ゲームファイル内のシェーダーを検索",
        "ステップ 2 / 4 · 保存済みシェーダーの再コンパイル",
        "ステップ 2 / 4 · シェーダーのコンパイル",
        "ステップ 3 / 4 · 中間形式の生成",
        "ステップ 4 / 4 · ソート済みインデックスの作成",
        "シェーダーを確認中...",
        "{} / {} シェーダー",
        "{} / {} MB · シェーダー {} 個を検出、新規 {} 個",
        " · 失敗 {} 個",
        "シェーダー {} 個をコンパイルしました",
        "残り2分未満",
        "残り約{}分",
        "残り約{}時間",
        "このステップ:経過 {}",
        " · 合計:{}",
        " (ステップ1の所要時間:{})",
    },
    {   // 简体中文 (TW2, 2026-11-14)
        ",", false,
        "正在准备着色器",
        "此过程只需进行一次,结果保存在 SD 卡上。完成后游戏将启动。\n可以随时关闭游戏:下次启动时会从中断处继续。",
        "第 1 步(共 4 步)· 优化已保存的着色器",
        "第 1 步(共 4 步)· 在游戏文件中查找着色器",
        "第 2 步(共 4 步)· 重新编译已保存的着色器",
        "第 2 步(共 4 步)· 编译着色器",
        "第 3 步(共 4 步)· 生成中间形式",
        "第 4 步(共 4 步)· 创建排序索引",
        "正在检查着色器...",
        "{} / {} 个着色器",
        "{} / {} MB · 找到 {} 个着色器,其中 {} 个为新增",
        " · {} 个失败",
        "已编译 {} 个着色器",
        "剩余不到 2 分钟",
        "剩余约 {} 分钟",
        "剩余约 {} 小时",
        "此步骤:已用 {}",
        " · 总计:{}",
        " (第 1 步用时 {})",
    },
    {   // Türkçe (TW2, 2026-11-15)
        ".", true,
        "Gölgelendiriciler hazırlanıyor",
        "Bir kez yapılır ve SD kartta saklanır. Bittiğinde oyun başlar.\nOyunu istediğiniz zaman kapatabilirsiniz: sonraki başlatma kaldığı yerden devam eder.",
        "Adım 1 / 4 · Önbellekteki gölgelendiriciler iyileştiriliyor",
        "Adım 1 / 4 · Oyun dosyalarındaki gölgelendiriciler bulunuyor",
        "Adım 2 / 4 · Önbellekteki gölgelendiriciler yeniden derleniyor",
        "Adım 2 / 4 · Gölgelendiriciler derleniyor",
        "Adım 3 / 4 · Ara biçimler oluşturuluyor",
        "Adım 4 / 4 · Sıralı dizin oluşturuluyor",
        "Gölgelendiriciler denetleniyor...",
        "{} / {} gölgelendirici",
        "{} / {} MB · {} gölgelendirici bulundu, {} yeni",
        " · {} başarısız",
        "{} gölgelendirici derlendi",
        "2 dakikadan az kaldı",
        "yaklaşık {} dakika kaldı",
        "yaklaşık {} saat kaldı",
        "Bu adım: şimdiye kadar {}",
        " · Toplam: {}",
        " (1. adım {} sürdü)",
    },
};

// tw2de (2026-10-25): a language pack's [port] prepass_* text over the base language's (cpu/tw2de_lang_pack.h).
// A format string only when it has as many "{}" as the base's (a wrong one would throw).
PrepassStrings PackPrepassStrings(PrepassStrings t) {
  auto holes = [](const char *s) {
    int n = 0;
    for (; *s; ++s)
      n += *s == '{' ? (s[1] == '}' ? 1 : 100) : 0;
    return n;
  };
  struct Field {
    const char *key;
    const char *PrepassStrings::*member;
  };
  static constexpr Field kFields[] = {
      {"prepass_thousands", &PrepassStrings::thousands}, {"prepass_title", &PrepassStrings::title},
      {"prepass_footer", &PrepassStrings::footer}, {"prepass_step_cached", &PrepassStrings::step_cached},
      {"prepass_step_scan", &PrepassStrings::step_scan}, {"prepass_step_recompile", &PrepassStrings::step_recompile},
      {"prepass_step_compile", &PrepassStrings::step_compile},
      {"prepass_step_front_end", &PrepassStrings::step_front_end},
      {"prepass_step_index", &PrepassStrings::step_index}, {"prepass_step_check", &PrepassStrings::step_check},
      {"prepass_n_of_total", &PrepassStrings::n_of_total}, {"prepass_scan_detail", &PrepassStrings::scan_detail},
      {"prepass_failed", &PrepassStrings::failed}, {"prepass_compiled", &PrepassStrings::compiled},
      {"prepass_eta_short", &PrepassStrings::eta_short}, {"prepass_eta_minutes", &PrepassStrings::eta_minutes},
      {"prepass_eta_hours", &PrepassStrings::eta_hours}, {"prepass_this_step", &PrepassStrings::this_step},
      {"prepass_total", &PrepassStrings::total}, {"prepass_step1_took", &PrepassStrings::step1_took},
  };
  for (const Field &f : kFields) {
    const char *v = tw2de::PortText(f.key, nullptr);
    if (v && holes(v) == holes(t.*f.member))
      t.*f.member = v;
  }
  if (const char *v = tw2de::PortText("prepass_decimal_comma", nullptr))
    t.decimal_comma = v[0] == 'y' || v[0] == '1' || v[0] == 't';
  return t;
}

std::string Thousands(const PrepassStrings &t, uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = int(s.size()) - 3; i > 0; i -= 3)
    s.insert(size_t(i), t.thousands);
  return s;
}

// The current step's remaining time (the screen says so: "this step ...").
std::string Minutes(const PrepassStrings &t, double seconds) {
  if (seconds < 90)
    return t.eta_short;
  if (seconds < 3600 * 2)
    return fmt::format(fmt::runtime(t.eta_minutes), int(seconds / 60 + 0.5));
  std::string hours = fmt::format("{:.1f}", seconds / 3600);
  if (t.decimal_comma)
    std::replace(hours.begin(), hours.end(), '.', ',');
  return fmt::format(fmt::runtime(t.eta_hours), hours);
}

// tw2de (2026-10-25): the applet-mode screen's text, in the console's language (it shows before the settings are
// read): title, two lines of what to do, the button.
struct AppletStrings {
  const char *title, *step, *detail, *eta, *footer;
};
constexpr AppletStrings kAppletStrings[kUiLanguageCount] = {
    {"Applet mode is not supported", "The game needs the memory only full application mode has.",
     "Hold R while you start any game from the HOME menu, then start tw2de",
     "from the homebrew menu that opens (not from the Album).", "Press any button to exit."},
    {"Der Applet-Modus wird nicht unterstützt",
     "Das Spiel braucht den Speicher, den nur der volle Anwendungsmodus bietet.",
     "R gedrückt halten, während ein beliebiges Spiel im HOME-Menü gestartet wird, dann tw2de",
     "aus dem sich öffnenden Homebrew-Menü starten (nicht aus dem Album).", "Eine beliebige Taste beendet."},
    {"Le mode applet n'est pas pris en charge",
     "Le jeu a besoin de la mémoire que seul le mode application complet fournit.",
     "Maintenez R en lançant n'importe quel jeu depuis le menu HOME, puis lancez tw2de",
     "depuis le menu homebrew qui s'ouvre (pas depuis l'Album).", "Appuyez sur un bouton pour quitter."},
    {"El modo applet no es compatible",
     "El juego necesita la memoria que solo ofrece el modo de aplicación completo.",
     "Mantén pulsado R al iniciar cualquier juego desde el menú HOME y luego inicia tw2de",
     "desde el menú homebrew que se abre (no desde el Álbum).", "Pulsa cualquier botón para salir."},
    {"La modalità applet non è supportata",
     "Il gioco richiede la memoria disponibile solo in modalità applicazione completa.",
     "Tieni premuto R mentre avvii un gioco qualsiasi dal menu HOME, poi avvia tw2de",
     "dal menu homebrew che si apre (non dall'Album).", "Premi un pulsante qualsiasi per uscire."},
    {   // Polski (TW2, 2026-11-14)
     "Tryb apletu nie jest obsługiwany",
     "Gra potrzebuje pamięci dostępnej tylko w pełnym trybie aplikacji.",
     "Przytrzymaj R, uruchamiając dowolną grę z menu HOME, a potem uruchom tw2de",
     "z menu homebrew, które się otworzy (nie z Albumu).",
     "Naciśnij dowolny przycisk, aby wyjść.",
    },
    {   // Čeština (TW2, 2026-11-14)
     "Režim appletu není podporován",
     "Hra potřebuje paměť, kterou má jen plný režim aplikace.",
     "Při spouštění libovolné hry z nabídky HOME držte R a pak spusťte tw2de",
     "z nabídky homebrew, která se otevře (ne z Alba).",
     "Stisknutím libovolného tlačítka ukončíte.",
    },
    {   // Magyar (TW2, 2026-11-14)
     "Az applet mód nem támogatott",
     "A játéknak olyan memória kell, amely csak teljes alkalmazás módban érhető el.",
     "Tartsd lenyomva az R gombot, miközben a HOME menüből elindítasz egy játékot, majd indítsd el a tw2de-t",
     "a megnyíló homebrew menüből (ne az Albumból).",
     "Kilépéshez nyomj meg egy gombot.",
    },
    {   // Русский (TW2, 2026-11-14)
     "Режим апплета не поддерживается",
     "Игре нужна память, доступная только в режиме полного приложения.",
     "Удерживайте R, запуская любую игру из меню HOME, затем запустите tw2de",
     "из открывшегося меню homebrew (не из Альбома).",
     "Нажмите любую кнопку для выхода.",
    },
    {   // 日本語 (TW2, 2026-11-14)
     "アプレットモードには対応していません",
     "このゲームには、フルアプリケーションモードでのみ使えるメモリが必要です。",
     "HOMEメニューで任意のゲームを起動するときにRを押し続け、開いたhomebrewメニューから",
     "tw2deを起動してください(アルバムからではなく)。",
     "いずれかのボタンを押すと終了します。",
    },
    {   // 简体中文 (TW2, 2026-11-14)
     "不支持小程序模式",
     "游戏需要只有完整应用程序模式才能提供的内存。",
     "在 HOME 菜单中启动任意游戏时按住 R,然后在打开的 homebrew 菜单中",
     "启动 tw2de(不要从相册启动)。",
     "按任意键退出。",
    },
    {   // Türkçe (TW2, 2026-11-15)
     "Applet modu desteklenmiyor",
     "Oyun, yalnızca tam uygulama modunun sağladığı belleğe ihtiyaç duyar.",
     "HOME menüsünden herhangi bir oyunu başlatırken R'yi basılı tutun, ardından tw2de'yi",
     "açılan homebrew menüsünden başlatın (Albüm'den değil).",
     "Çıkmak için herhangi bir düğmeye basın.",
    },
};

}  // namespace

bool RunningAsApplet() {
  const AppletType type = appletGetAppletType();
  return type != AppletType_Application && type != AppletType_SystemApplication;
}

// tw2de (2026-10-21): the applet-mode screen, before main (AppletModeGuard below): nothing of the SDK has run yet
// (no threads, no logging, no window), so nothing outlives the screen, and the process returns to hbloader the way
// libnx returns any homebrew (__nx_applet_exit_mode 0). The SDK's own exit asks the system to close an application
// (ISelfController::Exit), which in applet mode took the HOME menu down with it (qlaunch 2144-0001).
// Memory: an applet has about 250 MB with the NRO in it; the screen peaks around 25 MB (the 1920x1080 picture while
// it is decoded and scaled) and frees it all before returning.
static void ShowAppletModeError(const std::string &game_root) {
  PlFontData font{};
  const bool pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
  if (pl) {
    plGetSharedFontByType(&font, PlSharedFontType_Standard);
    renut::nxs::UseSystemFallbackFonts();  // TW2 (2026-11-14): Chinese
  }
  {
    PrepassScreen screen;
    const bool hd = ScreenWants1080p();  // tw2de (2026-11-07)
    screen.Init(game_root + "/nxeart", static_cast<const uint8_t *>(font.address), font.size, hd ? 1.5f : 1.0f);
    ScreenText text;
    const AppletStrings &a = kAppletStrings[SystemUiLanguage()];
    text.title = a.title;
    text.step = a.step;
    text.detail = a.detail;
    text.eta = a.eta;
    text.footer = a.footer;
    text.progress = -1.0f;
    ScreenFramebuffer sfb;
    const bool have_fb = sfb.Open(hd);
    Framebuffer &fb = sfb.fb;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    bool drawn = false;
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad))
        break;
      // The screen does not change: drawn into both buffers, then only waiting for a button.
      if (have_fb && !drawn) {
        for (int i = 0; i < 2; i++) {
          u32 stride = 0;
          auto *pixels = static_cast<uint32_t *>(framebufferBegin(&fb, &stride));
          screen.Draw(pixels, stride / 4, text);
          ScreenFramebufferEnd(fb);
        }
        drawn = true;
      }
      svcSleepThread(16'000'000);
    }
    sfb.Close();
  }
  if (pl)
    plExit();
}

namespace {
// Before main and before the SDK's constructors (101 is the first priority a program may use; the SDK's own at 101
// only installs a terminate handler). libnx's services are up: __appInit runs before the constructors.
__attribute__((constructor(101))) void AppletModeGuard() {
  if (!RunningAsApplet())
    return;
  ShowAppletModeError("sdmc:/switch/tw2de_nx/assets");
  // Not exit(): the SDK's crash hooks (switch_crash_hooks.c) register an atexit handler that aborts on any exit()
  // outside its own shutdown (diagAbortWithResult), and that abort of the album applet took the HOME menu down
  // (qlaunch, 2144-0001). __libnx_exit runs no atexit handlers: it closes the services and returns to hbloader
  // (__nx_applet_exit_mode 0), as any homebrew in applet mode.
  std::fflush(nullptr);
  __nx_applet_exit_mode = 0;
  __libnx_exit(0);
}
}  // namespace

void RunSwitchShaderPrepass(const std::string &game_root) {
  if (!REXCVAR_GET(tw2de_shader_prepass))
    return;

  static NxMutex log_mutex;
  static FILE *log_file = nullptr;
  PrepassConfig config;
  config.game_root = game_root;
  config.store_path = tw2de::renut_gpu::ShaderDataPath("tw2de_shaders.bin").string();
  config.threads = std::clamp(REXCVAR_GET(tw2de_prepass_threads), 1, 12);
  config.cores = {0, 1, 2};
  // Each compile thread: uam's tables for this thread first (a small vertex and pixel shader, on newlib's heap: they
  // live as long as the process), then its own allocator for the compiles (thread_alloc.h: newlib's malloc has one
  // lock, and uam calls it ~670,000 times a shader).
  config.compile_thread_start = [] {
    static const char vs[] = "#version 460\nlayout(location = 0) in vec4 pos;\nvoid main() { gl_Position = pos; }\n";
    static const char fs[] = "#version 460\nlayout(location = 0) out vec4 c;\nvoid main() { c = vec4(1.0); }\n";
    uam::compile(uam::Stage::Vertex, vs);
    uam::compile(uam::Stage::Fragment, fs);
    ThreadAllocBegin();
  };
  config.compile_thread_end = [] { ThreadAllocEnd(); };
  // tw2de (2026-10-21): the scan threads' translations too (XenosRecomp's strings and maps, the trim's): newlib's
  // one malloc lock had the three threads waiting for each other, the cores idle.
  config.scan_thread_start = [] { ThreadAllocBegin(); };
  config.scan_thread_end = [] { ThreadAllocEnd(); };
  config.translate_scope = [](bool on) { ThreadAllocSetActive(on); };
  config.have_dksh = [](uint64_t key) {
    static const bool recompile = REXCVAR_GET(tw2de_diag_prepass_recompile);  // tw2de (2026-11-08)
    return !recompile && tw2de::renut_gpu::deko3d::CacheHas(key);
  };
  config.dksh_fast = [](uint64_t key) { return tw2de::renut_gpu::deko3d::CacheIsFast(key); };
  config.retry_failed = REXCVAR_GET(tw2de_prepass_retry_failed);
  config.compile_peak = [] { return ThreadAllocScopePeak(); };  // tw2de (2026-11-08)
  // tw2de (2026-10-21): the variants played (recorded by the renderer), and a release's whole play-through.
  config.variant_lists = {tw2de::renut_gpu::ShaderDataPath("tw2de_variants.txt").string(),
                          tw2de::renut_gpu::ShaderDataPath("tw2de_variants_release.txt").string()};
  // TW2 (2026-11-15): with FSR 3.1 on (the boot menu has set it by now) the variants' motion-vector twins too
  // (2026-11-16: in no list, shader_prepass.h).
  config.motion = REXCVAR_GET(tw2de_fsr3) != 0;
  config.store_dksh = [](uint64_t key, const std::vector<uint8_t> &dksh) {
    // tw2de (2026-11-08): the recompile test keeps the cache as it is (the same results, and 600 MB more on the
    // card otherwise).
    static const bool recompile = REXCVAR_GET(tw2de_diag_prepass_recompile);
    if (!recompile)
      tw2de::renut_gpu::deko3d::CacheAdd(key, dksh);
  };
  // Each compile's wall time and the time its thread was on a core (svcGetInfo ThreadTickCount): wall well above the
  // CPU time means the threads wait (locks: the allocator, Mesa's tables), not compute. Logged every 100 compiles.
  static std::atomic<uint64_t> wall_ticks{0}, cpu_ticks{0}, measured{0}, window_start{0};
  config.compile = [&config](bool pixel, const std::string &glsl, std::vector<uint8_t> &dksh, std::string &log) {
    u64 expected = 0;
    window_start.compare_exchange_strong(expected, armGetSystemTick());  // the first compile starts the first window
    u64 cpu0 = 0, cpu1 = 0;
    svcGetInfo(&cpu0, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX);
    const u64 t0 = armGetSystemTick();
    uam::Result r;
    {
      ThreadAllocScope scope;  // uam's allocations from this thread's slabs
      r = uam::compile(pixel ? uam::Stage::Fragment : uam::Stage::Vertex, glsl.c_str());
    }
    const u64 t1 = armGetSystemTick();
    svcGetInfo(&cpu1, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX);
    wall_ticks += t1 - t0;
    cpu_ticks += cpu1 - cpu0;
    const uint64_t n = ++measured;
    if (n % 100 == 0) {
      const double freq = double(armGetSystemTickFreq());
      const double window = double(t1 - window_start.exchange(t1)) / freq;
      const double wall_ms = double(wall_ticks.exchange(0)) * 1000.0 / freq / 100.0;
      const double cpu_ms = double(cpu_ticks.exchange(0)) * 1000.0 / freq / 100.0;
      // tw2de (2026-11-08): and the memory (a full pre-pass ran out of it in uam near its end, std::bad_alloc in
      // nv50_ir's register allocation, three times in a row; resumed it finished): the process's used and total, newlib's
      // heap in use and its arena, the threads' slabs.
      u64 used = 0, total = 0;
      svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      const struct mallinfo mi = mallinfo();
      const ThreadAllocStats ts = GetThreadAllocStats();
      config.log(fmt::format("compile: {} done; the last 100 in {:.1f} s ({:.0f} a minute): {:.0f} ms a shader, {:.0f} ms "
                             "of it on a core ({:.0f} % waiting); memory: process {} of {} MB, newlib heap {} MB in use "
                             "of {} MB, slabs {} ({} MB), large blocks {}, remote frees {}; aligned to newlib: outside {}, "
                             "above 4 KB {}, odd {}, large {} (largest alignment {}, {} bytes)",
                             n, window, window > 0 ? 6000.0 / window : 0.0, wall_ms, cpu_ms,
                             wall_ms > 0 ? 100.0 * (1.0 - cpu_ms / wall_ms) : 0.0, used >> 20, total >> 20,
                             size_t(mi.uordblks) >> 20, size_t(mi.arena) >> 20, ts.slabs_live, ts.slab_bytes >> 20,
                             ts.blocks_large, ts.frees_remote, ts.align_inactive, ts.align_big, ts.align_odd,
                             ts.align_large, ts.align_max, ts.align_max_size));
    }
    // tw2de (2026-10-21): per thread, every 50 of its compiles: where its time went (thread_waits.h). A core
    // below 100 % has a thread that waits; this says on what.
    {
      struct Mine {
        uint32_t n = 0;
        u64 start = 0, wall = 0, cpu = 0;
        ThreadWaits at;
      };
      static thread_local Mine m;
      if (m.n == 0) {
        m.start = t0;
        m.at = MyWaits();
      }
      m.wall += t1 - t0;
      m.cpu += cpu1 - cpu0;
      if (++m.n == 50) {
        const ThreadWaits &w = MyWaits();
        const double per = 1.0 / 50.0;
        const double span = WaitTicksToMs(t1 - m.start);
        auto ms = [&](uint64_t a, uint64_t b) { return WaitTicksToMs(a - b) * per; };
        config.log(fmt::format(
            "thread on core {}: 50 compiles in {:.1f} s; a shader: {:.0f} ms compiling ({:.0f} on the core), outside "
            "it {:.0f} ms reading GLSL, {:.0f} ms adding DKSH; waited: our locks {:.1f} ms ({} times), pthread / "
            "Mesa locks {:.1f} ms ({}), newlib heap {:.1f} ms ({} calls; {:.1f} ms of it for its lock; outside the scope {}, "
            "over 512 KB {}, frees of newlib's blocks {}, reallocs of them {}, libc's own {}, aligned {})",
            svcGetCurrentProcessorNumber(), span / 1000.0, WaitTicksToMs(m.wall) * per, WaitTicksToMs(m.cpu) * per,
            ms(w.read, m.at.read), ms(w.store, m.at.store), ms(w.nx_lock, m.at.nx_lock), w.nx_waits - m.at.nx_waits,
            ms(w.pthread_lock, m.at.pthread_lock), w.pthread_waits - m.at.pthread_waits, ms(w.heap, m.at.heap),
            w.heap_calls - m.at.heap_calls, ms(w.heap_lock, m.at.heap_lock), w.heap_inactive - m.at.heap_inactive,
            w.heap_big - m.at.heap_big, w.heap_free_foreign - m.at.heap_free_foreign,
            w.heap_realloc_foreign - m.at.heap_realloc_foreign, w.heap_libc - m.at.heap_libc,
            w.heap_align - m.at.heap_align));
        m = {};
      }
    }
    dksh = std::move(r.dksh);
    log = std::move(r.log);
    return r.ok && !dksh.empty();
  };
  // Every draw of a shader with bool constants specialises them (tw2de_gpu_bool_spec): its default variant is never
  // used, so the pre-pass leaves it to the first draw.
  const bool bool_spec = REXCVAR_GET(tw2de_gpu_bool_spec);
  config.wanted = [bool_spec](bool, const uint32_t mask[8]) {
    if (!bool_spec)
      return true;
    for (int i = 0; i < 8; i++)
      if (mask[i])
        return false;
    return true;
  };
  // tw2de (2026-10-23, late specialisation: late_spec.h): every shader's front end, kept next to the store.
  // A store that could not be opened has nothing to make: no pre-pass at every start for it.
  config.have_front_end = [](uint64_t key) { return !FrontEndStore::Get().IsOpen() || FrontEndStore::Get().Has(key); };
  if (REXCVAR_GET(tw2de_deko3d_late_spec))
  config.front_end = [&config](bool pixel, const std::string &glsl, std::vector<uint8_t> &bytes, std::string &log) {
    std::string base;
    std::vector<std::pair<uint32_t, uint32_t>> defaults;
    if (!LateSpecBase(glsl, base, defaults))
      return false;
    const u64 t0 = armGetSystemTick();
    std::vector<uint8_t> fe;
    bool ok;
    {
      ThreadAllocScope scope;
      ok = uam::frontEnd(pixel ? uam::Stage::Fragment : uam::Stage::Vertex, base.c_str(), fe, log);
    }
    if (ok)
      LateSpecPack(defaults, fe, bytes);
    static std::atomic<uint64_t> n{0}, ticks{0};
    ticks += armGetSystemTick() - t0;
    if (++n % 500 == 0)
      config.log(fmt::format("front ends: {} done, {:.0f} ms each (the last 500)", n.load(),
                             double(ticks.exchange(0)) * 1000.0 / double(armGetSystemTickFreq()) / 500.0));
    return ok && !bytes.empty();
  };
  config.store_front_end = [](uint64_t key, const std::vector<uint8_t> &bytes) { FrontEndStore::Get().Add(key, bytes); };
  config.front_end_failed = [](uint64_t key) { FrontEndStore::Get().AddFailure(key); };
  // tw2de (2026-10-23): step 3: the last batches to the card, then the DKSH cache's index sorted and written.
  config.finish = [] {
    FrontEndStore::Get().Sync();
    ShaderStore::Get().Sync();
    tw2de::renut_gpu::deko3d::SaveShaderCache();
  };
  config.log = [](const std::string &line) {
    // tw2de (2026-10-25): the start check's timings in the game log only with log_diagnostics (shader_prepass.log
    // keeps every line).
    if (line.rfind("start check: ", 0) != 0 || REXCVAR_GET(log_diagnostics))
      REXLOG_INFO("[prepass] {}", line);
    std::lock_guard<NxMutex> lock(log_mutex);
    if (log_file) {
      std::fprintf(log_file, "%s\n", line.c_str());
      std::fflush(log_file);
    }
  };

  // The compiler as the renderer's thread sets it up (plume_deko3d.cpp compileWorker): the same flags, so the DKSH is
  // the one the game would compile.
  const u64 t0 = armGetSystemTick();
  uam::setFNXF_XIII_LRFlags(tw2de::renut_gpu::Deko3DUamFlags());
  uam::init(true);
  const u64 t1 = armGetSystemTick();
  const bool needed = PrepassNeeded(config);
  const u64 t2 = armGetSystemTick();
  const double tick_ms = 1000.0 / double(armGetSystemTickFreq());
  // tw2de (2026-10-23): the time before the game starts, step by step (PrepassNeeded's line has its parts).
  if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[prepass] start: compiler set up in {:.0f} ms, check in {:.0f} ms", double(t1 - t0) * tick_ms,
              double(t2 - t1) * tick_ms);
  if (!needed) {
    REXLOG_INFO("[prepass] nothing to do ({} shaders in the store)", ShaderStore::Get().Count());
    return;
  }
  // tw2de (2026-10-25): in log/, a new file each run (it was appended to).
  log_file = std::fopen(tw2de::renut_gpu::NextToExecutable("log/shader_prepass.log").string().c_str(), "w");
  config.log(fmt::format("---- shader pre-pass: game files {}, store {}", config.game_root, config.store_path));
  {
    u64 core_mask = 0;
    svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    s32 priority = 0;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    config.log(fmt::format("{} threads on cores 0-2; the process may use cores 0x{:X}; the main thread is on core {} at "
                           "priority 0x{:X}", config.threads, core_mask, svcGetCurrentProcessorNumber(), priority));
  }

  // The screen.
  PlFontData font{};
  const bool pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
  if (pl) {
    plGetSharedFontByType(&font, PlSharedFontType_Standard);
    renut::nxs::UseSystemFallbackFonts();  // TW2 (2026-11-14): Chinese
  }
  PrepassScreen screen;
  // tw2de (2026-11-07): 1920x1080 docked on a 1080p TV, switched as the console is docked or not (below).
  bool hd = ScreenWants1080p();
  screen.Init(game_root + "/nxeart", static_cast<const uint8_t *>(font.address), font.size, hd ? 1.5f : 1.0f);
  ScreenFramebuffer sfb;
  bool have_fb = sfb.Open(hd);
  Framebuffer &fb = sfb.fb;
  // tw2de (2026-10-21): the boost asked for again every 5 s for the whole pre-pass. Nothing in the NRO turns it
  // off before the end, but the system can drop it (a start was seen at normal clocks throughout): asking again is
  // one IPC call, and puts it back. Logged when the answer changes.
  Result boost_rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  config.log(R_SUCCEEDED(boost_rc) ? std::string("CPU boost asked for: accepted")
                                   : fmt::format("CPU boost asked for: refused (0x{:X})", boost_rc));
  u32 boost_tries = 1;
  u64 last_boost = armGetSystemTick();
  PrepassProgress progress;
  progress.skip_compile = !REXCVAR_GET(tw2de_prepass_compile);  // tw2de (2026-10-22): diagnostic
  std::thread worker([&] { RunPrepass(config, progress); });

  using SteadyClock = std::chrono::steady_clock;
  const SteadyClock::time_point start = SteadyClock::now();
  SteadyClock::time_point phase_start = start;
  double scan_seconds = 0.0;
  double front_end_seconds = 0.0;  // tw2de (2026-10-23): step 3, the intermediates
  int last_phase = -1;
  uint64_t base_done = 0;
  bool leaving = false;
  // The screen 4 times a second (HOME checked every 16 ms): a full-rate redraw (two 3.6 MB copies, the text rasterised and
  // the linear buffer swizzled into the framebuffer) took core 0 and memory bandwidth from the compile threads.
  u64 last_draw = 0;
  const u64 draw_every = armGetSystemTickFreq() / 4;
  while (progress.phase.load() != int(PrepassPhase::kDone)) {
    svcSleepThread(16'000'000);
    if (!appletMainLoop()) {
      // HOME closed the software: what is done stays; the next start goes on from there.
      progress.cancel = true;
      leaving = true;
    }
    // No way to skip it (tw2de, 2026-10-21): the game starts once every shader is compiled. Closing the software
    // from HOME keeps what is done; the next start goes on from there.
    // tw2de (2026-10-23): a new step is drawn at once (step 3 can be shorter than the redraw's quarter second).
    if (armGetSystemTick() - last_draw < draw_every && progress.phase.load() == last_phase)
      continue;
    last_draw = armGetSystemTick();
    if (last_draw - last_boost >= armGetSystemTickFreq() * 5) {
      last_boost = last_draw;
      const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
      ++boost_tries;
      if (rc != boost_rc)
        config.log(fmt::format("CPU boost asked for again (time {}): {} (0x{:X})", boost_tries,
                               R_SUCCEEDED(rc) ? "accepted" : "refused", rc));
      boost_rc = rc;
    }

    const int phase = progress.phase.load();
    if (phase != last_phase) {
      if (last_phase == int(PrepassPhase::kScan) || last_phase == int(PrepassPhase::kCached))
        scan_seconds += std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
      else if (last_phase == int(PrepassPhase::kFrontEnd))
        front_end_seconds += std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
      last_phase = phase;
      phase_start = SteadyClock::now();
      base_done = 0;
    }
    const double elapsed = std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
    const double total_elapsed = std::chrono::duration<double>(SteadyClock::now() - start).count();
    const PrepassStrings t = PackPrepassStrings(kPrepassStrings[UiLanguage()]);  // tw2de: a language pack's
    ScreenText text;
    text.title = t.title;
    text.footer = t.footer;
    if (phase == int(PrepassPhase::kCached)) {
      // tw2de (2026-10-23): the shaders found before, translated again for this version (container_cache.h).
      const uint32_t total = progress.cached_total.load(), done = progress.cached_done.load();
      text.step = t.step_cached;
      text.progress = total ? float(double(done) / double(total)) : 0.0f;
      text.detail = fmt::format(fmt::runtime(t.n_of_total), Thousands(t, done), Thousands(t, total));
      if (done >= 200 && elapsed > 3)
        text.eta = Minutes(t, elapsed * double(total - done) / double(done));
    }
    else if (phase == int(PrepassPhase::kScan)) {
      const uint64_t total = progress.bytes_total.load(), done = progress.scan.bytes.load();
      text.step = t.step_scan;
      text.progress = total ? float(double(done) / double(total)) : 0.0f;
      text.detail = fmt::format(fmt::runtime(t.scan_detail), Thousands(t, done >> 20), Thousands(t, total >> 20),
                                Thousands(t, progress.found.load()), Thousands(t, progress.added.load()));
      if (done > (64u << 20) && elapsed > 5)
        text.eta = Minutes(t, elapsed * double(total - done) / double(done));
    }
    else if (phase == int(PrepassPhase::kCompile)) {
      const uint32_t total = progress.compile_total.load(), done = progress.compile_done.load();
      // tw2de (2026-10-23): after the cached shaders were translated again (a new version), the compile is a
      // recompile of those.
      text.step = progress.cached_total.load() ? t.step_recompile : t.step_compile;
      text.progress = total ? float(double(done) / double(total)) : 0.0f;
      text.detail = fmt::format(fmt::runtime(t.n_of_total), Thousands(t, done), Thousands(t, total));
      if (progress.compile_failed.load())
        text.detail += fmt::format(fmt::runtime(t.failed), Thousands(t, progress.compile_failed.load()));
      if (done >= 30 && elapsed > 10)
        text.eta = Minutes(t, elapsed * double(total - done) / double(done - base_done));
    }
    else if (phase == int(PrepassPhase::kFrontEnd)) {
      // tw2de (2026-10-23): each shader's front end for late specialisation (late_spec.h): Mesa's intermediate
      // form, from which a variant compiles in about a quarter of the time.
      const uint32_t total = progress.front_end_total.load(), done = progress.front_end_done.load();
      text.step = t.step_front_end;
      text.progress = total ? float(double(done) / double(total)) : 0.0f;
      text.detail = fmt::format(fmt::runtime(t.n_of_total), Thousands(t, done), Thousands(t, total));
      if (done >= 30 && elapsed > 10)
        text.eta = Minutes(t, elapsed * double(total - done) / double(done));
    }
    else if (phase == int(PrepassPhase::kIndex)) {
      text.step = t.step_index;
      text.progress = -1.0f;
      text.detail = fmt::format(fmt::runtime(t.compiled), Thousands(t, progress.compile_done.load()));
    }
    else {
      text.step = t.step_check;
      text.progress = -1.0f;
    }
    // The timer: this step's time and the whole pre-pass's.
    // "This step: 3:12 so far, about 25 minutes left · Total: 4:40" (the estimate is this step's alone).
    {
      std::string line = fmt::format(fmt::runtime(t.this_step), Clock(elapsed));
      if (!text.eta.empty())
        line += ", " + text.eta;
      line += fmt::format(fmt::runtime(t.total), Clock(total_elapsed));
      if (phase == int(PrepassPhase::kCompile) && scan_seconds > 0.0)
        line += fmt::format(fmt::runtime(t.step1_took), Clock(scan_seconds));
      text.eta = line;
    }
    if (ScreenWants1080p() != hd) {  // tw2de (2026-11-07): docked or undocked
      hd = !hd;
      sfb.Close();
      screen.Init(game_root + "/nxeart", static_cast<const uint8_t *>(font.address), font.size, hd ? 1.5f : 1.0f);
      have_fb = sfb.Open(hd);
    }
    if (have_fb) {
      u32 stride = 0;
      auto *pixels = static_cast<uint32_t *>(framebufferBegin(&fb, &stride));
      screen.Draw(pixels, stride / 4, text);
      ScreenFramebufferEnd(fb);
    }
  }
  worker.join();
  const double total_seconds = std::chrono::duration<double>(SteadyClock::now() - start).count();
  if (last_phase == int(PrepassPhase::kScan) || last_phase == int(PrepassPhase::kCached))
    scan_seconds += std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
  else if (last_phase == int(PrepassPhase::kFrontEnd))
    front_end_seconds += std::chrono::duration<double>(SteadyClock::now() - phase_start).count();
  const double compile_seconds = last_phase == int(PrepassPhase::kCompile) || last_phase == int(PrepassPhase::kFrontEnd) ||
                                         last_phase == int(PrepassPhase::kIndex) || last_phase == int(PrepassPhase::kDone)
                                     ? std::max(0.0, total_seconds - scan_seconds - front_end_seconds)
                                     : 0.0;
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  ShaderStore::Get().Sync();
  tw2de::renut_gpu::deko3d::SaveShaderCache();
  const uint32_t compiled = progress.compile_done.load();
  {
    const ThreadAllocStats a = GetThreadAllocStats();
    config.log(fmt::format("compile threads' allocator: {} slabs ({} MB) still hold blocks, {} large blocks went to "
                           "newlib, {} blocks freed by another thread", a.slabs_live, a.slab_bytes >> 20,
                           a.blocks_large, a.frees_remote));
  }
  const uint32_t front_ends = progress.front_end_done.load();
  config.log(fmt::format("---- pre-pass {} in {}: finding the shaders {}, compiling {} ({} shaders, {:.0f} ms each on "
                         "{} threads), intermediates {} ({} shaders, {:.0f} ms each, {} without one); {} shaders "
                         "stored, {} failed to compile",
                         progress.cancel ? "stopped" : "done", Clock(total_seconds), Clock(scan_seconds),
                         Clock(compile_seconds), compiled,
                         compiled ? compile_seconds * 1000.0 * config.threads / compiled : 0.0, config.threads,
                         Clock(front_end_seconds), front_ends,
                         front_ends ? front_end_seconds * 1000.0 * config.threads / front_ends : 0.0,
                         progress.front_end_failed.load(), progress.stored.load(), progress.compile_failed.load()));
  sfb.Close();
  if (pl)
    plExit();
  if (log_file) {
    std::fclose(log_file);
    log_file = nullptr;
  }
  if (leaving)
    ExitBeforeGame();  // tw2de (2026-10-25): not std::exit (early_exit.h)
}

}  // namespace renut::nxs

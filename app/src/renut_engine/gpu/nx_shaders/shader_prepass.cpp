// tw2de (2026-10-21): shader_prepass.h.
#include "renut_engine/gpu/nx_shaders/shader_prepass.h"
#include "renut_engine/gpu/nx_shaders/front_end_store.h"
#include "renut_engine/gpu/nx_shaders/nx_file.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"
#include "renut_engine/gpu/nx_shaders/thread_waits.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <pthread.h>
#if defined(__SWITCH__)
#include <switch.h>
#include <malloc.h>

extern "C" char *fake_heap_start;  // libnx: newlib's heap
extern "C" char *fake_heap_end;
#endif
#include <xxhash.h>

#include "renut_engine/gpu/nx_shaders/shader_scan.h"
#include "renut_engine/gpu/nx_shaders/shader_store.h"
#include "renut_engine/gpu/nx_shaders/container_cache.h"
#include "renut_engine/gpu/nx_shaders/variant_list.h"
#include "renut_engine/gpu/nx_shaders/xenos_glsl.h"

namespace renut::nxs {

namespace {
// tw2de (2026-11-08): the shaders being compiled now (RunPrepass's compile step: one compile of each at a time).
std::mutex g_in_flight_mutex;
std::condition_variable g_in_flight_cv;
std::unordered_set<uint64_t> g_in_flight;
}  // namespace

namespace {

inline uint32_t Be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// Runs fn(i) for i in [0, threads) on threads with config's stack and cores; returns when all are done.
// The Switch: libnx threads made on their core (threadCreate's cpuid), not moved there afterwards: a pthread starts
// on the default core, and when the move did not take, every worker shared one core (more threads, no more CPU).
// Each thread logs the core it runs on.
#if defined(__SWITCH__)
void RunThreads(const PrepassConfig &config, const std::function<void(int)> &fn) {
  struct Arg {
    const PrepassConfig *config;
    const std::function<void(int)> *fn;
    int index, core;
  };
  const int n = std::max(1, config.threads);
  std::vector<Thread> threads(static_cast<size_t>(n));
  std::vector<Arg> args(static_cast<size_t>(n));
  std::vector<bool> started(static_cast<size_t>(n), false);
  for (int i = 0; i < n; i++) {
    const int core = config.cores.empty() ? -2 : config.cores[size_t(i) % config.cores.size()];
    args[size_t(i)] = {&config, &fn, i, core};
    // 0x2C: the main thread's priority. The stack from the heap (libnx allocates it), config.stack_size.
    Result rc = threadCreate(
        &threads[size_t(i)],
        [](void *p) {
          const Arg &a = *static_cast<Arg *>(p);
          if (a.config->log)
            a.config->log(fmt::format("thread {}: on core {} (asked for {})", a.index,
                                      svcGetCurrentProcessorNumber(), a.core));
          (*a.fn)(a.index);
        },
        &args[size_t(i)], nullptr, config.stack_size, 0x2C, core);
    if (R_SUCCEEDED(rc))
      rc = threadStart(&threads[size_t(i)]);
    started[size_t(i)] = R_SUCCEEDED(rc);
    if (R_FAILED(rc) && config.log)
      config.log(fmt::format("thread {}: could not be started on core {} (0x{:X})", i, core, rc));
  }
  for (int i = 0; i < n; i++) {
    if (started[size_t(i)]) {
      threadWaitForExit(&threads[size_t(i)]);
      threadClose(&threads[size_t(i)]);
    }
  }
}
#else
void RunThreads(const PrepassConfig &config, const std::function<void(int)> &fn) {
  struct Arg {
    const PrepassConfig *config;
    const std::function<void(int)> *fn;
    int index;
  };
  const int n = std::max(1, config.threads);
  std::vector<pthread_t> threads(static_cast<size_t>(n));
  std::vector<Arg> args(static_cast<size_t>(n));
  std::vector<bool> started(static_cast<size_t>(n), false);
  for (int i = 0; i < n; i++) {
    args[size_t(i)] = {&config, &fn, i};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, config.stack_size);
    started[size_t(i)] = pthread_create(
                             &threads[size_t(i)], &attr,
                             [](void *p) -> void * {
                               const Arg &a = *static_cast<Arg *>(p);
                               if (a.config->pin_thread && !a.config->cores.empty())
                                 a.config->pin_thread(a.config->cores[size_t(a.index) % a.config->cores.size()]);
                               (*a.fn)(a.index);
                               return nullptr;
                             },
                             &args[size_t(i)]) == 0;
    pthread_attr_destroy(&attr);
  }
  for (int i = 0; i < n; i++)
    if (started[size_t(i)])
      pthread_join(threads[size_t(i)], nullptr);
}
#endif

}  // namespace

// The console: fsFsOpenDirectory + fsDirRead (nx_file.h), the names and sizes in batches.
std::vector<std::pair<std::string, uint64_t>> ListGameFiles(const std::string &root) {
  std::vector<std::pair<std::string, uint64_t>> files;
  for (const NxDirEntry &e : NxListFiles(root, ".bin"))
    if (e.size)
      files.emplace_back(e.path, e.size);
  // TW2 (2026-11-12): TW2 keeps its shaders as plain D3D shader containers (0x102A1100 / 0x102A1101
  // headers) in Data/shader.cache (about 3,000) and Data/staticshader.cache (137): the shader caches only, not the
  // texture / mesh / animation caches.
  for (const NxDirEntry &e : NxListFiles(root, "shader.cache"))
    if (e.size)
      files.emplace_back(e.path, e.size);
  return files;
}

namespace {

// The variant lists merged (the recorded one first; a release's lines it lacks after), each variant once.
// TW2 (2026-11-16): plain variants only: a motion-vector line (an older list's) is taken as its plain form, without
// a key (the line's was the twin's); the twin of a default variant leaves nothing. A line with its key replaces the
// same variant's line without one.
std::vector<VariantEntry> MergedVariants(const PrepassConfig &config) {
  std::vector<VariantEntry> all;
  std::unordered_map<std::string, size_t> at;
  for (const std::string &path : config.variant_lists) {
    for (VariantEntry &e : LoadVariantList(path)) {
      if (StripMotion(e) && e.values.empty())
        continue;
      const auto [it, fresh] = at.emplace(e.Identity(), all.size());
      if (fresh)
        all.push_back(std::move(e));
      else if (!all[it->second].tag && e.tag)
        all[it->second] = std::move(e);
    }
  }
  return all;
}

// TW2 (2026-11-16; the user: "i wanted text file to contain only standard ones, adding motion vector twin sounds
// like waste of space"): the recorded list written again without its motion-vector lines (the lists of 2026-11-15
// had one for every variant and every shader), each kept as its plain form when the list lacks it. At the start
// check, before anything reads the list.
void PlainListOnly(const PrepassConfig &config) {
  if (config.variant_lists.empty())
    return;
  const std::string &path = config.variant_lists.front();
  std::vector<VariantEntry> lines = LoadVariantList(path);
  size_t motion = 0;
  for (const VariantEntry &e : lines)
    motion += MotionVariant(e) ? 1 : 0;
  if (!motion)
    return;
  std::vector<VariantEntry> out;
  std::unordered_map<std::string, size_t> at;
  for (VariantEntry &e : lines) {
    if (StripMotion(e) && e.values.empty())
      continue;
    const auto [it, fresh] = at.emplace(e.Identity(), out.size());
    if (fresh)
      out.push_back(std::move(e));
    else if (!out[it->second].tag && e.tag)
      out[it->second] = std::move(e);
  }
  if (!SaveVariantList(path, out))
    return;  // tried again at the next start
  if (config.log)
    config.log(fmt::format("variant list: {} motion-vector lines taken out (the list holds plain variants only), "
                           "{} lines left",
                           motion, out.size()));
}

// TW2 (2026-11-15, PrepassConfig::motion; 2026-11-16: in no list): the motion-vector twins of the lists' variants
// and of the default variants the pre-pass builds: the same constants with constant 914's bit 2 set, keyed from
// their plain form's key (MotionTwinKey: no text is read for it). A listed variant's only once its key is this
// translation's.
std::vector<VariantEntry> MotionTwins(const PrepassConfig &config, ShaderStore &store,
                                      const std::vector<VariantEntry> &listed) {
  std::vector<VariantEntry> twins;
  if (!config.motion)
    return twins;
  std::unordered_set<std::string> seen;
  auto add = [&](uint64_t hash, bool pixel, std::vector<std::pair<uint32_t, uint32_t>> values, uint64_t plain_key) {
    auto it = std::find_if(values.begin(), values.end(), [](const auto &v) { return v.first == kVariantWordFx; });
    if (it != values.end())
      it->second |= kVariantMotionBit;
    else
      values.insert(std::find_if(values.begin(), values.end(), [](const auto &v) { return v.first > kVariantWordFx; }),
                    std::make_pair(uint32_t(kVariantWordFx), uint32_t(kVariantMotionBit)));
    VariantEntry t;
    t.tag = TranslatorTag();
    t.key = MotionTwinKey(plain_key);
    t.hash = hash;
    t.pixel = pixel;
    t.values = std::move(values);
    if (seen.insert(t.Identity()).second)
      twins.push_back(std::move(t));
  };
  for (const VariantEntry &e : listed)
    if (e.tag == TranslatorTag() && store.Find(e.hash))
      add(e.hash, e.pixel, e.values, e.key);
  store.ForEach([&](const StoredShader &s) {
    if (!config.wanted || config.wanted(s.pixel, s.boolMask))
      add(s.entry.hash, s.pixel, {}, s.dkKey);
  });
  return twins;
}

// A variant the pre-pass compiles: its shader is stored, and its DKSH is missing or a fast pass's (a line of another
// translation: its key is computed again first).
bool VariantWanted(const PrepassConfig &config, ShaderStore &store, const VariantEntry &e) {
  if (!store.Find(e.hash))
    return false;  // not translated yet (a shader of the game's executable, before the game made it)
  if (e.tag != TranslatorTag())
    return true;
  if (store.Failed(e.key) && !config.retry_failed)
    return false;
  return !config.have_dksh(e.key) || (config.dksh_fast && config.dksh_fast(e.key));
}

// tw2de (2026-10-23): the container cache (container_cache.h) next to the store.
ContainerCache &OpenContainerCache(const PrepassConfig &config) {
  ContainerCache &cache = ContainerCache::Get();
  cache.Open((std::filesystem::path(config.store_path).parent_path() / "tw2de_shader_containers").string());
  return cache;
}

// tw2de (2026-10-23): the front-end store (front_end_store.h) next to the store.
void OpenFrontEndStore(const PrepassConfig &config) {
  if (config.front_end)
    FrontEndStore::Get().Open((std::filesystem::path(config.store_path).parent_path() / "tw2de_shaders_fe").string());
}

}  // namespace

namespace {
// tw2de (2026-11-10): the translations from the container cache, summed over the threads (RunPrepass).
struct CachedStats {
  std::atomic<uint64_t> translate{0}, add{0}, nx_lock{0}, pthread_lock{0}, heap{0}, heap_lock{0}, last{0};
  std::atomic<uint32_t> heap_calls{0};
};
CachedStats g_cached_stats;
}  // namespace

bool PrepassNeeded(const PrepassConfig &config) {
  // tw2de (2026-10-23): each step's time in the log (the start's wait before the game shows).
  using Clock = std::chrono::steady_clock;
  Clock::time_point t = Clock::now();
  std::string times;
  auto step = [&](const char *what) {
    const Clock::time_point now = Clock::now();
    times += fmt::format("{}{} {} ms", times.empty() ? "" : ", ", what,
                         std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count());
    t = now;
  };
  struct Report {
    const PrepassConfig &config;
    std::string &times;
    ~Report() {
      if (config.log)
        config.log("start check: " + times);
    }
  } report{config, times};
  ShaderStore &store = ShaderStore::Get();
  const bool store_ok = store.Open(config.store_path);
  step("shader store");
  if (!store_ok)
    return false;
  PlainListOnly(config);  // TW2 (2026-11-16): before the lists are read
  ContainerCache &cache = OpenContainerCache(config);
  step("container index");
  OpenFrontEndStore(config);
  step("front-end index");
  const std::filesystem::path root(config.game_root);
  const auto files = ListGameFiles(config.game_root);
  step(fmt::format("listing {} game files", files.size()).c_str());
  for (const auto &[path, size] : files) {
    const std::string name = std::filesystem::path(path).lexically_relative(root).generic_string();
    if (!store.FileScanned(name, size) && !cache.FileScanned(name, size))
      return true;
  }
  // tw2de (2026-10-23): containers found before that this translation has not translated yet.
  for (const uint64_t hash : cache.Hashes())
    if (!store.Find(hash) && !store.Failed(hash))
      return true;
  if (!config.compile || !config.have_dksh)
    return false;
  config.have_dksh(0);  // the DKSH cache loaded (its own line has its time)
  step("DKSH cache");
  bool needed = false;
  store.ForEach([&](const StoredShader &s) {
    if (!needed && (!config.wanted || config.wanted(s.pixel, s.boolMask)) && !store.Failed(s.entry.hash) &&
        (!config.have_dksh(s.dkKey) || (config.dksh_fast && config.dksh_fast(s.dkKey))))
      needed = true;
  });
  // tw2de (2026-10-23): a shader without its front end (late specialisation).
  if (!needed && config.front_end && config.have_front_end) {
    store.ForEach([&](const StoredShader &s) {
      if (!needed && !config.have_front_end(s.dkKey))
        needed = true;
    });
  }
  // The lists' variants, and with FSR 3.1 on (TW2, 2026-11-15) their motion-vector twins and the default variants'
  // (in no list: MotionTwins).
  if (!needed) {
    const std::vector<VariantEntry> merged = MergedVariants(config);
    for (const VariantEntry &e : merged) {
      if (VariantWanted(config, store, e)) {
        needed = true;
        break;
      }
    }
    if (!needed) {
      for (const VariantEntry &e : MotionTwins(config, store, merged)) {
        if (VariantWanted(config, store, e)) {
          needed = true;
          break;
        }
      }
    }
  }
  step("shaders and variants checked");
  return needed;
}

void RunPrepass(const PrepassConfig &config, PrepassProgress &progress) {
  auto log = [&](const std::string &line) {
    if (config.log)
      config.log(line);
  };
  ShaderStore &store = ShaderStore::Get();
  if (!store.Open(config.store_path)) {
    log(fmt::format("shader store {}: cannot be opened", config.store_path));
    progress.phase = int(PrepassPhase::kDone);
    return;
  }
  progress.stored = uint32_t(store.Count());
  log(fmt::format("shader store {}: {} shaders", config.store_path, store.Count()));

  OpenFrontEndStore(config);
  // ---- tw2de (2026-10-23): the containers found before (another translation's store, or a pre-pass cut short),
  // translated from the container cache: no game file is read again for them.
  ContainerCache &cache = OpenContainerCache(config);
  {
    uint32_t wanted = 0;
    for (const uint64_t hash : cache.Hashes())
      wanted += !store.Find(hash) && !store.Failed(hash) ? 1u : 0u;
    progress.cached_total = wanted;
    if (wanted)
      progress.phase = int(PrepassPhase::kCached);
    const size_t done = cache.ForEachContainer(
        [&](uint64_t hash) { return !store.Find(hash) && !store.Failed(hash); },
        [&](uint64_t hash, const uint8_t *c, size_t size) {
          const uint32_t n_done = progress.cached_done.fetch_add(1) + 1;
          // tw2de (2026-11-10): where this step's time goes (a line every 1000 shaders): translating, adding to the
          // store, and the waits inside them (our locks, pthread / Mesa locks, newlib's heap and its lock). The CPU
          // sat mostly idle while all 25,163 stored shaders were translated again (translator version 8).
          const ThreadWaits w0 = MyWaits();
          const uint64_t t0 = WaitTicks();
          TranslatedShader t;
          if (config.translate_scope)
            config.translate_scope(true);
          const bool ok = Translate(c, size, t);
          if (config.translate_scope)
            config.translate_scope(false);
          const uint64_t t1 = WaitTicks();
          const bool added = ok && store.Add(t);
          const uint64_t t2 = WaitTicks();
          {
            const ThreadWaits &w = MyWaits();
            g_cached_stats.translate += t1 - t0;
            g_cached_stats.add += t2 - t1;
            g_cached_stats.nx_lock += w.nx_lock - w0.nx_lock;
            g_cached_stats.pthread_lock += w.pthread_lock - w0.pthread_lock;
            g_cached_stats.heap += w.heap - w0.heap;
            g_cached_stats.heap_lock += w.heap_lock - w0.heap_lock;
            g_cached_stats.heap_calls += w.heap_calls - w0.heap_calls;
            if (n_done % 1000 == 0) {
              const uint64_t now = WaitTicks();
              const uint64_t last = g_cached_stats.last.exchange(now);
              auto ms = [](std::atomic<uint64_t> &a) { return WaitTicksToMs(a.exchange(0)) / 1000.0; };
              const double tr = ms(g_cached_stats.translate), ad = ms(g_cached_stats.add);
              const double nx = ms(g_cached_stats.nx_lock), pt = ms(g_cached_stats.pthread_lock);
              const double hp = ms(g_cached_stats.heap), hl = ms(g_cached_stats.heap_lock);
              log(fmt::format("cached: {} done; the last 1000 in {:.1f} s; thread-seconds: translating {:.1f}, adding to "
                              "the store {:.1f}; of them waiting: our locks {:.1f}, pthread / Mesa locks {:.1f}, newlib "
                              "heap {:.1f} ({} calls; its lock {:.1f})",
                              n_done, last ? WaitTicksToMs(now - last) / 1000.0 : 0.0, tr, ad, nx, pt, hp,
                              g_cached_stats.heap_calls.exchange(0), hl));
            }
          }
          if (added) {
            progress.added++;
            progress.stored++;
          }
          else {
            progress.translate_failed++;
            store.AddFailure(hash);
            log(fmt::format("shader {:016X}: does not translate", hash));
          }
        },
        [&](const std::function<void(int)> &body) {
          RunThreads(config, [&](int i) {
            if (config.scan_thread_start)
              config.scan_thread_start();
            body(i);
            if (config.scan_thread_end)
              config.scan_thread_end();
          });
        });
    if (done) {
      store.Sync();
      log(fmt::format("container cache: {} of its {} containers translated (no game file read for them)", done,
                      cache.Count()));
    }
  }

  // ---- phase 1: the game files (the step shown only when there are files to read: tw2de, 2026-10-23).
  const auto files = ListGameFiles(config.game_root);
  // tw2de (2026-10-21): each file read whole into memory (one card operation), then its 32 MB ranges scanned
  // there by every thread; one thread reads the next file meanwhile, while the budget allows. (A whole file per
  // thread left one thread scanning the 1.6 GB file alone at the end.) A file there is no block for is scanned
  // in ranges read from the card, as before.
  constexpr uint64_t kMemoryBudget = 2600ull << 20;
  // tw2de (2026-11-10): FF XIII-2 scans every file in ranges from the card (each thread's one range buffer,
  // shader_scan.cpp). Its whole files (up to 1.8 GB) fragmented newlib's heap until no 36 MB block was left: the
  // first run ended in std::bad_alloc, the second gave up five files ("could not be read") and its compiles then
  // crawled in newlib's fragmented heap.
  // tw2de (2026-11-10, later): read in 512 MB pieces into slots reserved once (below).
  constexpr uint64_t kHeadroom = 1024ull << 20;
  std::vector<size_t> jobs;
  const std::filesystem::path root(config.game_root);
  std::vector<std::string> names(files.size());
  uint64_t total = 0;
  for (size_t i = 0; i < files.size(); i++) {
    names[i] = std::filesystem::path(files[i].first).lexically_relative(root).generic_string();
    if (store.FileScanned(names[i], files[i].second) || cache.FileScanned(names[i], files[i].second)) {
      progress.files_skipped++;
      continue;
    }
    jobs.push_back(i);
    total += files[i].second;
  }
  std::stable_sort(jobs.begin(), jobs.end(), [&](size_t a, size_t b) { return files[a].second > files[b].second; });
  if (!jobs.empty())
    progress.phase = int(PrepassPhase::kScan);
  progress.files_total = uint32_t(files.size());
  progress.files_done = progress.files_skipped.load();
  progress.bytes_total = total;
  log(fmt::format("{} game files, {} already scanned; {} MB to scan, in {} MB ranges of 512 MB pieces "
                  "(largest file {} MB)",
                  files.size(), progress.files_skipped.load(), total >> 20, kScanChunk >> 20,
                  jobs.empty() ? 0 : files[jobs.front()].second >> 20));

  if (!jobs.empty()) {
    NxMutex seen_mutex;
    std::unordered_set<uint64_t> seen;
    seen.reserve(32768);  // The game has 18,791: no rehash while the threads wait on its lock
    ScanStats &stats = progress.scan;
    // tw2de (2026-11-10): where the scan's time goes (a line every 30 s): the files' bytes, the zlib streams,
    // the shaders found and new, and the threads' time translating them.
    std::atomic<uint64_t> translate_ns{0};
    // tw2de (2026-11-10): and the rest of the sink: the container cache, the store, the waits in all of it.
    struct {
      std::atomic<uint64_t> cache{0}, add{0}, nx_lock{0}, pthread_lock{0}, heap{0}, heap_lock{0};
      std::atomic<uint32_t> heap_calls{0}, inactive{0}, big{0}, free_foreign{0}, realloc_foreign{0}, libc{0}, align{0},
          slab_fail{0};
    } scan_w;
    const auto scan_start = std::chrono::steady_clock::now();
    auto last_report = scan_start;
    const ContainerSink sink = [&](const uint8_t *c, size_t size) {
      // As XenosRecomp's reader (main.cpp): headers with these words set are not shaders it takes.
      if (Be32(c + 0x1C) != 0 || Be32(c + 0x20) != 0)
        return;
      const uint64_t hash = XXH3_64bits(c, size);
      {
        std::lock_guard<NxMutex> lock(seen_mutex);
        if (!seen.insert(hash).second)
          return;
      }
      const ThreadWaits w0 = MyWaits();
      const uint64_t c0 = WaitTicks();
      cache.Add(hash, c, size);  // tw2de (2026-10-23): for the next translation
      scan_w.cache += WaitTicks() - c0;
      progress.found++;
      if (store.Find(hash) || store.Failed(hash))
        return;
      TranslatedShader t;
      bool ok;
      const auto tr_start = std::chrono::steady_clock::now();
      {
        // The translation's many small blocks from the thread's own allocator (no newlib lock), when there is one.
        if (config.translate_scope)
          config.translate_scope(true);
        ok = Translate(c, size, t);
        if (config.translate_scope)
          config.translate_scope(false);
      }
      translate_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - tr_start).count()),
                             std::memory_order_relaxed);
      const uint64_t a0 = WaitTicks();
      const bool added = ok && store.Add(t);
      scan_w.add += WaitTicks() - a0;
      {
        const ThreadWaits &w = MyWaits();
        scan_w.nx_lock += w.nx_lock - w0.nx_lock;
        scan_w.pthread_lock += w.pthread_lock - w0.pthread_lock;
        scan_w.heap += w.heap - w0.heap;
        scan_w.heap_lock += w.heap_lock - w0.heap_lock;
        scan_w.heap_calls += w.heap_calls - w0.heap_calls;
        scan_w.inactive += w.heap_inactive - w0.heap_inactive;
        scan_w.big += w.heap_big - w0.heap_big;
        scan_w.free_foreign += w.heap_free_foreign - w0.heap_free_foreign;
        scan_w.realloc_foreign += w.heap_realloc_foreign - w0.heap_realloc_foreign;
        scan_w.libc += w.heap_libc - w0.heap_libc;
        scan_w.align += w.heap_align - w0.heap_align;
        scan_w.slab_fail += w.slab_fail - w0.slab_fail;
      }
      if (added) {
        progress.added++;
        progress.stored++;
      }
      else {
        progress.translate_failed++;
        store.AddFailure(hash);
        log(fmt::format("shader {:016X}: does not translate", hash));
      }
    };

    // tw2de (2026-11-10, later still): the files in 512 MB pieces, each loaded into one of kSlots slots of
    // one block reserved here once (kPiece + kPieceMargin each: the margin holds a raw container that starts before
    // the piece's end; a zlib stream past the margin goes on from the file, ScanPiece). The 32 MB ranges of a piece
    // are scanned by every thread from memory; one thread loads the next piece meanwhile, while a slot is free. No
    // allocation per file or piece: nothing to fragment.
    constexpr uint64_t kPiece = 512ull << 20;
    constexpr uint64_t kPieceMargin = 4ull << 20;
    constexpr uint64_t kSlotSize = kPiece + kPieceMargin;
    constexpr int kSlots = 2;  // one loading while one is scanned
    uint8_t *block = nullptr;
    int slots_n = 0;
    for (int n = kSlots; n >= 1 && !block; n--) {
      block = static_cast<uint8_t *>(std::malloc(size_t(kSlotSize * uint64_t(n))));
      if (!block)
        continue;
      // The rest of the pre-pass (translating, the store) needs the heap too: kHeadroom left beside the block.
      if (void *room = std::malloc(size_t(kHeadroom))) {
        std::free(room);
        slots_n = n;
      }
      else {
        std::free(block);
        block = nullptr;
      }
    }
    // tw2de (2026-11-10): newlib's heap, its whole size, what malloc took of it and what is in use (with 3
    // slots, the translations' slabs were refused: 35,000 times in 30 s, everything then from newlib under its lock).
    const auto heap_line = [&]() {
      const struct mallinfo mi = mallinfo();
      return fmt::format("heap {} MB, of it taken by malloc {} MB, in use {} MB, free in it {} MB",
                         uint64_t(fake_heap_end - fake_heap_start) >> 20, uint64_t(mi.arena) >> 20,
                         uint64_t(mi.uordblks) >> 20, uint64_t(mi.fordblks) >> 20);
    };
    log(fmt::format("scan: {} slots of {} MB for the files' pieces; {}", slots_n, kSlotSize >> 20, heap_line()));
    std::vector<int> free_slots;
    for (int i = slots_n - 1; i >= 0; i--)
      free_slots.push_back(i);

    // The pieces, in the files' order (largest file first), and per file the pieces left and whether one failed.
    struct PieceJob {
      size_t fi;
      uint64_t at, len;
    };
    std::vector<PieceJob> pieces;
    std::unique_ptr<std::atomic<uint32_t>[]> pieces_left(new std::atomic<uint32_t>[files.size()]);
    std::unique_ptr<std::atomic<bool>[]> file_failed(new std::atomic<bool>[files.size()]);
    for (size_t i = 0; i < files.size(); i++) {
      pieces_left[i] = 0;
      file_failed[i] = false;
    }
    for (size_t fi : jobs) {
      const uint64_t fsize = files[fi].second;
      uint32_t n = 0;
      for (uint64_t at = 0; at < fsize || (fsize == 0 && n == 0); at += kPiece, n++)
        pieces.push_back({fi, at, std::min(kPiece, fsize - at)});
      pieces_left[fi] = n;
    }

    // A piece being scanned: its bytes (null: read from the card per range, when there is no slot at all) and the
    // ranges not handed out yet.
    struct Loaded {
      PieceJob job;
      int slot = -1;
      const uint8_t *data = nullptr;
      size_t data_len = 0;
      uint64_t next = 0;  // in the file
      std::atomic<uint32_t> ranges_left{0};
    };
    NxMutex mutex;
    std::condition_variable_any wake;
    std::deque<std::shared_ptr<Loaded>> ready;  // pieces with ranges not handed out
    size_t next_load = 0;
    bool loading = false;
    // The file's mark once its last piece is scanned (under no lock: the counters are atomic).
    const auto piece_done = [&](size_t fi) {
      if (pieces_left[fi].fetch_sub(1) != 1)
        return;
      if (!file_failed[fi]) {
        store.MarkFileScanned(names[fi], files[fi].second);
        cache.MarkFileScanned(names[fi], files[fi].second);
      }
      progress.files_done++;
    };
    RunThreads(config, [&](int) {
      if (config.scan_thread_start)
        config.scan_thread_start();
      std::unique_lock<NxMutex> lock(mutex);
      for (;;) {
        if (progress.cancel)
          break;
        // The card first: the next piece read while the others scan (one at a time, into a free slot).
        if (!loading && next_load < pieces.size() && (!free_slots.empty() || slots_n == 0)) {
          auto f = std::make_shared<Loaded>();
          f->job = pieces[next_load++];
          if (slots_n) {
            f->slot = free_slots.back();
            free_slots.pop_back();
          }
          loading = true;
          lock.unlock();
          const std::string &path = files[f->job.fi].first;
          const uint64_t fsize = files[f->job.fi].second;
          ScanResult r = ScanResult::kOk;
          const auto load_start = std::chrono::steady_clock::now();
          if (f->slot >= 0 && f->job.len) {
            f->data_len = size_t(std::min(fsize, f->job.at + f->job.len + kPieceMargin) - f->job.at);
            uint8_t *mem = block + uint64_t(f->slot) * kSlotSize;
            r = LoadPiece(path, f->job.at, mem, f->data_len, &progress.cancel);
            if (r == ScanResult::kOk)
              f->data = mem;
          }
          const double load_s =
              std::chrono::duration<double>(std::chrono::steady_clock::now() - load_start).count();
          lock.lock();
          loading = false;
          if (r != ScanResult::kOk) {
            if (f->slot >= 0)
              free_slots.push_back(f->slot);
            if (!progress.cancel)
              log(fmt::format("{}: could not be read (at {} MB)", path, f->job.at >> 20));
            file_failed[f->job.fi] = true;
            lock.unlock();
            piece_done(f->job.fi);
            lock.lock();
            wake.notify_all();
            continue;
          }
          if (f->data && f->job.len >= (64ull << 20))
            log(fmt::format("{} at {} MB: {} MB read in {:.1f} s ({:.1f} MB/s)", path, f->job.at >> 20,
                            f->data_len >> 20, load_s, load_s > 0 ? double(f->data_len >> 20) / load_s : 0.0));
          const uint32_t n = uint32_t((f->job.len + kScanChunk - 1) / kScanChunk);
          f->ranges_left = n;
          f->next = f->job.at;
          if (n == 0) {
            if (f->slot >= 0)
              free_slots.push_back(f->slot);
            lock.unlock();
            piece_done(f->job.fi);
            lock.lock();
          }
          else {
            ready.push_back(std::move(f));
          }
          wake.notify_all();
          continue;
        }
        if (const auto now = std::chrono::steady_clock::now(); now - last_report >= std::chrono::seconds(30)) {
          last_report = now;
          const double s = std::chrono::duration<double>(now - scan_start).count();
          log(fmt::format("scan: {:.0f} s, {} of {} MB ({:.1f} MB/s), {} files done, {} zlib streams, {} shaders "
                          "found ({} new), translating {:.0f} thread-s",
                          s, stats.bytes.load() >> 20, total >> 20, double(stats.bytes.load() >> 20) / s,
                          progress.files_done.load(), stats.streams.load(), progress.found.load(),
                          progress.added.load(), double(translate_ns.load()) / 1e9));
          auto sec = [](std::atomic<uint64_t> &a) { return WaitTicksToMs(a.load()) / 1000.0; };
          log(fmt::format("scan: thread-s: container cache {:.1f}, store {:.1f}; waiting: our locks {:.1f}, pthread / "
                          "Mesa locks {:.1f}, newlib heap {:.1f} ({} calls; its lock {:.1f}; outside the scope {}, over "
                          "512 KB {}, frees of newlib's blocks {}, reallocs of them {}, libc's own {}, aligned {}; "
                          "slabs not given {})",
                          sec(scan_w.cache), sec(scan_w.add), sec(scan_w.nx_lock), sec(scan_w.pthread_lock),
                          sec(scan_w.heap), scan_w.heap_calls.load(), sec(scan_w.heap_lock), scan_w.inactive.load(),
                          scan_w.big.load(), scan_w.free_foreign.load(), scan_w.realloc_foreign.load(),
                          scan_w.libc.load(), scan_w.align.load(), scan_w.slab_fail.load()));
          log("scan: " + heap_line());
        }
        if (!ready.empty()) {
          std::shared_ptr<Loaded> f = ready.front();
          const uint64_t piece_end = f->job.at + f->job.len;
          const uint64_t lo = f->next, hi = std::min(piece_end, lo + kScanChunk);
          f->next = hi;
          if (hi >= piece_end)
            ready.pop_front();
          lock.unlock();
          const std::string &path = files[f->job.fi].first;
          const bool ok = f->data ? ScanPiece(path, files[f->job.fi].second, f->job.at, f->data, f->data_len, lo, hi,
                                              sink, stats, &progress.cancel)
                                  : ScanFileRange(path, lo, hi, sink, stats, &progress.cancel);
          if (!ok) {
            if (!progress.cancel && !file_failed[f->job.fi].exchange(true))
              log(fmt::format("{}: could not be read", path));
            file_failed[f->job.fi] = true;
          }
          if (f->ranges_left.fetch_sub(1) == 1) {
            // The piece's last range: its slot back, and the file's mark if it was the file's last piece.
            piece_done(f->job.fi);
            lock.lock();
            if (f->slot >= 0)
              free_slots.push_back(f->slot);
            f->data = nullptr;
            wake.notify_all();
            continue;
          }
          lock.lock();
          continue;
        }
        if (!loading && next_load >= pieces.size())
          break;  // nothing to read, nothing to hand out: the others finish their ranges
        wake.wait(lock);
      }
      lock.unlock();
      wake.notify_all();
      if (config.scan_thread_end)
        config.scan_thread_end();
    });
    std::free(block);
    store.Sync();
    cache.Sync();
    log(fmt::format("scan: {} containers ({} new, {} failed to translate), {} zlib streams",
                    progress.found.load(), progress.added.load(), progress.translate_failed.load(),
                    stats.streams.load()));
  }
  if (progress.cancel) {
    progress.phase = int(PrepassPhase::kDone);
    return;
  }

  // ---- phase 2: each shader's default variant through uam.
  progress.phase = int(PrepassPhase::kCompile);
  if (!config.compile || !config.have_dksh || !config.store_dksh || progress.skip_compile) {
    progress.phase = int(PrepassPhase::kDone);
    return;
  }
  // A compile: a stored shader's default variant, or a recorded variant of it.
  struct Work {
    const StoredShader *s;
    const VariantEntry *v;  // null: the default variant
    bool front_end = false;  // tw2de (2026-10-23): the shader's front end (late_spec.h), not a compile
    bool pixel() const { return s->pixel; }
  };
  std::vector<Work> todo;
  todo.reserve(store.Count());  // sized once
  uint32_t cached = 0;
  store.ForEach([&](const StoredShader &s) {
    if (config.wanted && !config.wanted(s.pixel, s.boolMask))
      return;
    if (store.Failed(s.entry.hash))
      return;  // uam failed on it before: not tried at every start (the game compiles it again if it is drawn)
    // a fast pass's DKSH (or one an older uam made worse: the DKSH cache's generation) is compiled in full again
    if (config.have_dksh(s.dkKey) && !(config.dksh_fast && config.dksh_fast(s.dkKey)))
      cached++;
    else
      todo.push_back({&s, nullptr});
  });
  // Vertex shaders first (few, and a draw needs one), then the pixel shaders.
  std::stable_sort(todo.begin(), todo.end(), [](const Work &a, const Work &b) { return !a.pixel() && b.pixel(); });
  const size_t defaults = todo.size();

  // tw2de (2026-10-21): the variants played (variant_list.h): the recorded list and a release's, merged; the lines
  // of another translation get this one's keys (their GLSL read); the list written back with them.
  std::vector<VariantEntry> variants = MergedVariants(config);
  bool keys_changed = false;
  size_t variants_cached = 0;
  const bool glsl_in_memory = !variants.empty() || !todo.empty();
  size_t in_memory = 0;
  if (glsl_in_memory) {
    // tw2de (2026-10-21): the GLSL from memory, not a card read per shader (it waited behind the DKSH cache's
    // batch writes: cores 1 and 2 at 20-50 %).
    in_memory = store.LoadGlslIntoMemory();
  }
  if (!variants.empty()) {
    std::string glsl;
    for (VariantEntry &e : variants) {
      const StoredShader *s = store.Find(e.hash);
      if (!s)
        continue;
      if (e.tag != TranslatorTag()) {
        if (!store.ReadGlsl(*s, glsl))
          continue;
        const std::string source = VariantSource(glsl, e.values);
        e.key = Fnv1a64(source.data(), source.size());
        e.tag = TranslatorTag();
        keys_changed = true;
      }
      if (VariantWanted(config, store, e))
        todo.push_back({s, &e});
      else
        variants_cached++;
    }
  }
  // TW2 (2026-11-15, PrepassConfig::motion; 2026-11-16: not lines of the list): with FSR 3.1 on, the motion-vector
  // twins of the lists' variants and of the default variants, compiled as the variants are.
  std::vector<VariantEntry> twins = MotionTwins(config, store, variants);
  if (!twins.empty()) {
    size_t twins_todo = 0;
    for (VariantEntry &e : twins) {
      const StoredShader *s = store.Find(e.hash);
      if (!s)
        continue;
      if (VariantWanted(config, store, e)) {
        todo.push_back({s, &e});
        twins_todo++;
      } else {
        variants_cached++;
      }
    }
    if (config.log)
      config.log(fmt::format("FSR 3.1: {} motion-vector twins (of {} listed variants and the default variants), {} of "
                             "them to compile; none is a line of the list",
                             twins.size(), variants.size(), twins_todo));
  }
  // The recorded list written again with this translation's keys.
  if (keys_changed && !config.variant_lists.empty())
    SaveVariantList(config.variant_lists.front(), variants);
  // tw2de (2026-11-08): the variants first, the shaders' defaults after them: a variant compiles faster than a
  // default, and with the slow defaults first the screen's time left (from the average so far) said 40-70 minutes
  // for a step that took under 30.
  std::rotate(todo.begin(), todo.begin() + ptrdiff_t(defaults), todo.end());
  // tw2de (2026-10-23): the front ends last (the compiles are what a start without them misses most), as a step of
  // their own (kFrontEnd, "Generating intermediates"): its own count and progress on the screen.
  std::vector<Work> front_end_todo;
  if (config.front_end && config.have_front_end && config.store_front_end) {
    store.ForEach([&](const StoredShader &s) {
      if (!config.have_front_end(s.dkKey))
        front_end_todo.push_back({&s, nullptr, true});
    });
  }
  const size_t front_ends = front_end_todo.size();
  const bool glsl_needed = !todo.empty() || !front_end_todo.empty();
  if (glsl_needed && !glsl_in_memory)
    in_memory = store.LoadGlslIntoMemory();
  progress.front_end_total = uint32_t(front_ends);
  progress.compile_cached = cached;
  progress.compile_total = uint32_t(todo.size());
  log(fmt::format("compile: {} shaders, {} already compiled; {} variants played (of {} listed, {} compiled in full "
                  "already); {} front ends for late specialisation",
                  defaults, cached, todo.size() - defaults, variants.size(), variants_cached, front_ends));
  if (glsl_needed)
    log(in_memory ? fmt::format("compile: the shaders' GLSL in memory ({} MB, compressed)", (in_memory + (1u << 19)) >> 20)
                  : std::string("compile: the shaders' GLSL could not be read into memory; read from the card"));
  if (!todo.empty()) {
    std::atomic<size_t> next{0};
    RunThreads(config, [&](int) {
      if (config.compile_thread_start)
        config.compile_thread_start();
      {
      std::string glsl, ulog;
      std::vector<uint8_t> dksh;
      for (;;) {
        if (progress.cancel || progress.skip_compile)
          break;
        const size_t i = next.fetch_add(1);
        if (i >= todo.size())
          break;
        const StoredShader &s = *todo[i].s;
        const VariantEntry *v = todo[i].v;
        ThreadWaits &waits = MyWaits();
        uint64_t t0 = WaitTicks();
        bool ok = store.ReadGlsl(s, glsl);
        waits.read += WaitTicks() - t0;
        uint64_t key = s.dkKey;
        if (ok && v) {
          glsl = VariantSource(glsl, v->values);
          key = v->key;
        }
        if (ok) {
          dksh.clear();
          ulog.clear();
          // tw2de (2026-11-08): one compile of a shader at a time: its variants are next to each other in the
          // list, and the heaviest (vertex shader BE95EE803D59E654, 555 MB of uam's memory each, 6 s) made by three
          // threads at once would want 1.7 GB.
          {
            std::unique_lock<std::mutex> lock(g_in_flight_mutex);
            g_in_flight_cv.wait(lock, [&] { return !g_in_flight.count(s.entry.hash); });
            g_in_flight.insert(s.entry.hash);
          }
          const uint64_t c0 = WaitTicks();
          ok = config.compile(s.pixel, glsl, dksh, ulog);
          {
            std::lock_guard<std::mutex> lock(g_in_flight_mutex);
            g_in_flight.erase(s.entry.hash);
          }
          g_in_flight_cv.notify_all();
          // tw2de (2026-11-08): a compile that took much memory, named (a full pre-pass had one of ~1 GB).
          if (config.compile_peak) {
            const uint64_t peak = config.compile_peak();
            if (peak >= (96ull << 20))
              log(fmt::format("compile: shader {:016X} ({}{}, GLSL {} KB): {} MB at most, {:.1f} s{}", s.entry.hash,
                              s.pixel ? "pixel" : "vertex", v ? fmt::format(", variant {:016X}", key) : "",
                              glsl.size() >> 10, peak >> 20, WaitTicksToMs(WaitTicks() - c0) / 1000.0,
                              ok ? "" : ", failed"));
          }
          if (ok) {
            t0 = WaitTicks();
            config.store_dksh(key, dksh);  // a fast pass's record replaced
            waits.store += WaitTicks() - t0;
          }
        }
        if (!ok) {
          progress.compile_failed++;
          // Recorded: the pre-pass does not wait on it at the next start (a variant by its key: the shader's other
          // variants and its default are still compiled).
          store.AddFailure(v ? key : s.entry.hash);
          log(fmt::format("shader {:016X}{}: uam failed: {}", s.entry.hash, v ? fmt::format(" variant {:016X}", key) : "",
                          ulog.substr(0, 400)));
          // tw2de (2026-11-10): the GLSL kept (the first 200 failures of a run) next to the store, in failed/,
          // for the translator's fixes: FF XIII-2's first full run had 57 (r32 and iTexCoord0 undeclared).
          static std::atomic<uint32_t> dumped{0};
          if (!config.store_path.empty() && dumped.fetch_add(1) < 200) {
            std::error_code ec;
            const auto dir = std::filesystem::path(config.store_path).parent_path() / "failed";
            std::filesystem::create_directories(dir, ec);
            if (FILE *f = std::fopen((dir / fmt::format("{:016X}{}.glsl", s.entry.hash,
                                                        v ? fmt::format("_{:016X}", key) : "")).string().c_str(),
                                     "wb")) {
              std::fwrite(glsl.data(), 1, glsl.size(), f);
              std::fprintf(f, "\n/* uam:\n%s\n*/\n", ulog.c_str());
              std::fclose(f);
            }
          }
        }
        progress.compile_done++;
      }
      }  // the thread's buffers go before its allocator does
      if (config.compile_thread_end)
        config.compile_thread_end();
    });
  }
  if (glsl_in_memory || glsl_needed)
    store.ReleaseGlslMemory();
  log(fmt::format("compile: {} done, {} failed", progress.compile_done.load(), progress.compile_failed.load()));
  // tw2de (2026-10-23): the front ends, a step of their own.
  if (!front_end_todo.empty() && !progress.cancel && !progress.skip_compile) {
    progress.phase = int(PrepassPhase::kFrontEnd);
    std::atomic<size_t> next{0};
    RunThreads(config, [&](int) {
      if (config.compile_thread_start)
        config.compile_thread_start();
      {
      std::string glsl, ulog;
      std::vector<uint8_t> fe_bytes;
      for (;;) {
        if (progress.cancel || progress.skip_compile)
          break;
        const size_t i = next.fetch_add(1);
        if (i >= front_end_todo.size())
          break;
        const StoredShader &s = *front_end_todo[i].s;
        ThreadWaits &waits = MyWaits();
        const uint64_t t0 = WaitTicks();
        const bool ok = store.ReadGlsl(s, glsl);
        waits.read += WaitTicks() - t0;
        // Kept, or its failure recorded (not tried at every start).
        fe_bytes.clear();
        ulog.clear();
        if (ok && config.front_end(s.pixel, glsl, fe_bytes, ulog)) {
          config.store_front_end(s.dkKey, fe_bytes);
        } else {
          progress.front_end_failed++;
          if (config.front_end_failed)
            config.front_end_failed(s.dkKey);
        }
        progress.front_end_done++;
      }
      }  // the thread's buffers go before its allocator does
      if (config.compile_thread_end)
        config.compile_thread_end();
    });
    log(fmt::format("front ends: {} done, {} without one", progress.front_end_done.load(),
                    progress.front_end_failed.load()));
  }
  if (config.finish) {
    progress.phase = int(PrepassPhase::kIndex);
    const auto t0 = std::chrono::steady_clock::now();
    config.finish();
    log(fmt::format("sorted index: written in {} ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
  }
  progress.phase = int(PrepassPhase::kDone);
}

}  // namespace renut::nxs

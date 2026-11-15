// tw2de (2026-10-21): pgo_switch.h. Built only with TW2DE_PGO=generate.
//
// tw2de (2026-10-25): the counts are written by this file, not by libgcov. A whole run left the pgo folder empty:
// libgcov writes one .gcda file per object (about 44 000 here, each opened, read, merged and written again on the SD
// card), says nothing when one fails (its messages go to stderr) and a dump did not finish within the time HOME gives a
// closing app. Here each object registers itself with us instead of libgcov (the link wraps __gcov_init, so libgcov's
// own list stays empty and it writes nothing), and a dump serialises every object's .gcda image with libgcov's
// __gcov_info_to_gcda (GCC 12+, the same bytes it would write) into ONE file per run:
//   /switch/tw2de_nx/pgo/run_<start time>.fxpg   ("FXPG", version 2, object count, then per object: name length,
//                                                   name, data length, data; little-endian u32s)
// tw2de (2026-10-26): version 2: functions numbered inside their file (--param=profile-func-internal-id=1,
// app/CMakeLists.txt), not by a hash of their source's path; version 1's runs do not fit a build of today.
// written as .tmp and renamed over the previous one, so an interrupted dump leaves the last complete one. The counters
// are never reset: each dump holds the whole run so far. Every dump logs its size, time and any failure in the game
// log. On the PC, build_switch.py --pgo use unpacks pgo/*.fxpg into .gcda files (tools/pgo_unpack.py), merging
// several runs with gcov-tool.
#include "pgo_switch.h"

#include <rex/logging.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <switch.h>

struct gcov_info;
extern "C" void __gcov_info_to_gcda(const gcov_info *gi_ptr, void (*filename_fn)(const char *, void *),
                                    void (*dump_fn)(const void *, unsigned, void *),
                                    void *(*allocate_fn)(unsigned, void *), void *arg);

namespace {

// tw2de (2026-10-30): TW2DE_PGO_RUN_DIR (app/CMakeLists.txt): /switch/tw2de_nx/pgo, or pgo-o2 for the -O2 build.
#ifdef TW2DE_PGO_RUN_DIR
constexpr const char *kDir = TW2DE_PGO_RUN_DIR;
#else
constexpr const char *kDir = "/switch/tw2de_nx/pgo";
#endif

// Every instrumented object's profile record, from its constructor (before main: no locks needed then, but the list is
// guarded anyway for objects of libraries started later).
std::mutex &InfoMutex() {
  static std::mutex m;
  return m;
}
std::vector<const gcov_info *> &Infos() {
  static std::vector<const gcov_info *> v;
  return v;
}

}  // namespace

// The link wraps __gcov_init (app/CMakeLists.txt, TW2DE_PGO=generate): libgcov's own registration is not called.
extern "C" void __wrap___gcov_init(const gcov_info *info) {
  if (!info)
    return;
  std::lock_guard<std::mutex> lock(InfoMutex());
  Infos().push_back(info);
}

namespace tw2de {

namespace {

std::mutex &PgoMutex() {
  static std::mutex m;
  return m;
}

std::string &RunPath() {
  static std::string path;
  return path;
}

// One object's image while __gcov_info_to_gcda produces it.
struct Object {
  std::string name;
  std::vector<uint8_t> data;
};

void OnFilename(const char *name, void *arg) { static_cast<Object *>(arg)->name = name ? name : ""; }
void OnData(const void *data, unsigned length, void *arg) {
  auto &out = static_cast<Object *>(arg)->data;
  const auto *bytes = static_cast<const uint8_t *>(data);
  out.insert(out.end(), bytes, bytes + length);
}
void *OnAllocate(unsigned length, void *) { return std::malloc(length); }

bool PutU32(FILE *f, uint32_t v) {
  const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  return std::fwrite(b, 1, 4, f) == 4;
}

// 2026-10-22: closing from the HOME menu ends the process without the app's shutdown (the counts since the last
// periodic dump were lost). appletLockExit makes Horizon wait for the app and report the request as an applet hook,
// which the window's appletMainLoop runs: the counts are written there.
AppletHookCookie g_exit_cookie;

void OnAppletEvent(AppletHookType hook, void *) {
  if (hook != AppletHookType_OnExitRequest)
    return;
  PgoDump();
  appletUnhook(&g_exit_cookie);
  // tw2de (2026-10-24): the exit is not let go here: the SDK holds it for the app's own shutdown
  // (windowed_app_context_switch.cpp), and ExitProcess lets it go.
}

}  // namespace

void PgoDump(const char *part) {
  std::lock_guard<std::mutex> lock(PgoMutex());
  const auto t0 = std::chrono::steady_clock::now();
  mkdir("/switch", 0777);
  mkdir("/switch/tw2de_nx", 0777);
  mkdir(kDir, 0777);
  if (RunPath().empty())
    RunPath() = std::string(kDir) + "/run_" + std::to_string(uint64_t(std::time(nullptr))) + ".fxpg";
  // tw2de (2026-10-26): a part of the run (pgo_switch.h): run_<start>.<part>.fxpg, the counts so far.
  const std::string path =
      part ? RunPath().substr(0, RunPath().size() - 5) + "." + part + ".fxpg" : RunPath();
  const std::string tmp = path + ".tmp";

  std::vector<const gcov_info *> infos;
  {
    std::lock_guard<std::mutex> l(InfoMutex());
    infos = Infos();
  }
  FILE *f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    REXLOG_ERROR("[pgo] cannot create {}: errno {} ({})", tmp, errno, std::strerror(errno));
    return;
  }
  static std::vector<char> buffer(4u << 20);
  std::setvbuf(f, buffer.data(), _IOFBF, buffer.size());
  bool ok = std::fwrite("FXPG", 1, 4, f) == 4 && PutU32(f, 2) && PutU32(f, uint32_t(infos.size()));
  uint64_t bytes = 12;
  std::string first_name;
  Object object;
  for (const gcov_info *info : infos) {
    if (!ok)
      break;
    object.name.clear();
    object.data.clear();
    __gcov_info_to_gcda(info, OnFilename, OnData, OnAllocate, &object);
    if (first_name.empty())
      first_name = object.name;
    ok = PutU32(f, uint32_t(object.name.size())) &&
         std::fwrite(object.name.data(), 1, object.name.size(), f) == object.name.size() &&
         PutU32(f, uint32_t(object.data.size())) &&
         std::fwrite(object.data.data(), 1, object.data.size(), f) == object.data.size();
    bytes += 8 + object.name.size() + object.data.size();
  }
  const int write_errno = errno;
  if (std::fclose(f) != 0)
    ok = false;
  if (!ok) {
    REXLOG_ERROR("[pgo] writing {} failed after {:.1f} MB: errno {} ({}); the previous dump is kept", tmp,
                 double(bytes) / 1048576.0, write_errno, std::strerror(write_errno));
    std::remove(tmp.c_str());
    return;
  }
  std::remove(path.c_str());  // FAT: no rename over an existing file
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    REXLOG_ERROR("[pgo] renaming {} failed: errno {} ({})", tmp, errno, std::strerror(errno));
    return;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[pgo] {} objects, {:.1f} MB -> {} in {:.0f} ms (first: {})", infos.size(), double(bytes) / 1048576.0,
              path, ms, first_name);
}

// tw2de (2026-10-26): written when the game closes only (OnShutdown, and the HOME menu's exit through the applet
// hook): no more dump every 2 minutes. The glitches came back with the working dumps (before them the pgo folder
// stayed empty and the run was clean): each one serialised 44,826 profiles (11 MB) and wrote them to the SD card in
// ~0.8 s, on a thread of its own, in the middle of the game's streaming. A run that crashes keeps nothing now.
void StartPgoDumper() {
  static std::atomic<bool> started{false};
  if (started.exchange(true))
    return;
  appletHook(&g_exit_cookie, OnAppletEvent, nullptr);  // the exit is locked by the SDK at start
}

}  // namespace tw2de

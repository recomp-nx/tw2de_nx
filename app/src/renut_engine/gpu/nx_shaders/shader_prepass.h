// tw2de (2026-10-21): the shader pre-pass, before the game starts. Phase 1 scans the game files for shader
// containers (shader_scan.h) and translates the new ones into the store (shader_store.h, xenos_glsl.h); phase 2
// compiles each stored shader's default variant with uam into the DKSH cache the deko3d back end reads. Both are
// resumable: the store marks each game file once all its shaders are in, and the cache holds every compiled shader,
// so a pre-pass cut short (HOME, power) goes on from where it was.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "renut_engine/gpu/nx_shaders/shader_scan.h"

namespace renut::nxs {

// tw2de (2026-10-23): kCached, the containers found before translated again (container_cache.h); kIndex, the
// last batch written and the sorted index of the compiled shaders made (PrepassConfig::finish).
enum class PrepassPhase : int { kStart, kCached, kScan, kCompile, kFrontEnd, kIndex, kDone };

struct PrepassProgress {
  std::atomic<int> phase{int(PrepassPhase::kStart)};
  // Scan.
  std::atomic<uint64_t> bytes_total{0};
  ScanStats scan;  // .bytes: done so far
  std::atomic<uint32_t> files_total{0}, files_done{0}, files_skipped{0};
  std::atomic<uint32_t> found{0}, added{0}, translate_failed{0};
  // tw2de (2026-10-23): the cached containers translated again (kCached).
  std::atomic<uint32_t> cached_total{0}, cached_done{0};
  // Compile.
  std::atomic<uint32_t> compile_total{0}, compile_done{0}, compile_failed{0}, compile_cached{0};
  // tw2de (2026-10-23): the front ends (kFrontEnd, after the compiles); failed: no late form or uam failed.
  std::atomic<uint32_t> front_end_total{0}, front_end_done{0}, front_end_failed{0};
  std::atomic<uint32_t> stored{0};  // shaders in the store
  std::atomic<bool> cancel{false};  // stop after the current shaders (what is done stays)
  std::atomic<bool> skip_compile{false};
};

struct PrepassConfig {
  std::string game_root;            // the game files (.bin, recursively)
  std::string store_path;           // shader_store.h's file
  int threads = 3;
  std::vector<int> cores{0, 1, 2};  // thread i runs on cores[i % size]
  size_t stack_size = 8u << 20;     // uam's parser and nv50_ir recurse deeply
  // The DKSH cache (tw2de_renut_deko3d.cpp) and uam (null: no compile phase).
  std::function<bool(uint64_t key)> have_dksh;
  std::function<bool(bool pixel, const std::string &glsl, std::vector<uint8_t> &dksh, std::string &log)> compile;
  // tw2de (2026-11-08): the memory the calling thread's last compile took at most (bytes; null: not measured).
  std::function<uint64_t()> compile_peak;
  std::function<void(uint64_t key, const std::vector<uint8_t> &dksh)> store_dksh;
  // The shaders whose default variant the compile phase builds (null: all). With tw2de_gpu_bool_spec every draw of a
  // shader with bool constants specialises them, so its default variant is never used.
  std::function<bool(bool pixel, const uint32_t bool_mask[8])> wanted;
  std::function<void(int core)> pin_thread;  // the platform's core affinity (null: none)
  // Around each compile thread's work (the Switch: uam's warm-up, then its own allocator; shader_prepass_switch.cpp).
  std::function<void()> compile_thread_start, compile_thread_end;
  // tw2de (2026-10-21): around each scan thread's work, and around each translation (true / false): the Switch
  // gives the translations the thread's own allocator (thread_alloc.h), as the compiles have.
  std::function<void()> scan_thread_start, scan_thread_end;
  std::function<void(bool)> translate_scope;
  // tw2de (2026-10-21): the variant lists (variant_list.h): the first is the one the game records into, written
  // again with the current translation's keys; the others (a release's) are read only. Their variants of stored
  // shaders whose DKSH is missing or a fast pass's (dksh_fast) are compiled in full after the default variants.
  std::vector<std::string> variant_lists;
  // TW2 (2026-11-15, FSR 3.1; the user: "can't you get from the variant text files those shaders to compile twice,
  // one with and one without motion vectors?"): 'motion' (FSR 3.1 on): every variant of the lists and every default
  // variant is compiled also in its motion-vector form (specialization constant 914 with bit 2 set: xenos_glsl.cpp
  // MotionVectorPass), so that turning FSR 3.1 on compiles nothing at run time of what was played without it.
  // TW2 (2026-11-16, the user: "i wanted text file to contain only standard ones, adding motion vector twin sounds
  // like waste of space"): the twins are in no list. The lists hold plain variants only (a variant first met with
  // FSR 3.1 on is recorded as its plain form; an older list's motion-vector lines are taken out at the start
  // check), and a twin's DKSH key is made from its plain form's key (variant_list.h MotionTwinKey).
  bool motion = false;
  std::function<bool(uint64_t key)> dksh_fast;
  // tw2de (2026-10-30): a diagnostic: variants recorded as failed are compiled again (tw2de_prepass_retry_failed).
  bool retry_failed = false;
  std::function<void(const std::string &line)> log;
  // tw2de (2026-10-23): after the compiles (phase kIndex): what is buffered to the card, and the DKSH cache's sorted
  // index (null: nothing).
  std::function<void()> finish;
  // tw2de (2026-10-23, late specialisation: late_spec.h): every stored shader's front end, kept by the store
  // (front_end_store.h), made in the compile phase after the compiles. front_end: the shader's GLSL to the bytes kept
  // (false: none, recorded so it is not tried again). Null: none made.
  std::function<bool(uint64_t key)> have_front_end;
  std::function<bool(bool pixel, const std::string &glsl, std::vector<uint8_t> &bytes, std::string &log)> front_end;
  std::function<void(uint64_t key, const std::vector<uint8_t> &bytes)> store_front_end;
  std::function<void(uint64_t key)> front_end_failed;
};

// Runs the pre-pass on config.threads threads; returns when it is done or cancelled.
void RunPrepass(const PrepassConfig &config, PrepassProgress &progress);

// Whether RunPrepass has anything to do: a game file not scanned yet, or a wanted shader not compiled. Quick (the
// store and the cache's index); opens the store.
bool PrepassNeeded(const PrepassConfig &config);

// Lists the game files the scan reads (sorted), with their sizes.
std::vector<std::pair<std::string, uint64_t>> ListGameFiles(const std::string &root);

}  // namespace renut::nxs

/**
 * @file    gpu/backend/memory_report.cpp
 */
#include "renut_engine/gpu/backend/memory_report.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif
#if defined(__SWITCH__)
#include <malloc.h>
#include <unistd.h>
#include <switch.h>
extern "C" size_t __nx_heap_size;  // libnx; the SDK's runtime_switch.cpp sets it
extern "C" char *fake_heap_end;    // libnx: the heap's end (sbrk grows towards it)
// tw2de (2026-11-05): newlib's malloc (devkitA64 libc.a, mallocr.c): its bins, whose third word is the top chunk
// (the heap's free end, where sbrk room is added), and its lock.
extern "C" void *__malloc_av_[];
extern "C" void __malloc_lock(struct _reent *);
extern "C" void __malloc_unlock(struct _reent *);
// tw2de (2026-10-26): the guest memory's backing, taken from the heap as the game commits (sdk/src/core/
// guest_memory_switch.cpp; never given back).
extern "C" size_t RexGmCommittedBytes(void);
extern "C" size_t RexGmChunkCount(void);
// tw2de (2026-10-26): the guest threads' host stacks, how deep each live one went (sdk/src/core/threading_posix.cpp).
extern "C" size_t RexSwitchStackDepths(size_t *used, size_t *sizes, size_t max);
#endif

#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-24): sdk/src/system/xobject.cpp

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/render/shader_library.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/backend/video.h"
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // tw2de (2026-10-26): GetDeko3DStats
#endif
#if defined(__SWITCH__)
#include "renut_engine/gpu/nx_shaders/thread_alloc.h"  // tw2de (2026-10-26): HeapSitesReport
#endif

REXCVAR_DEFINE_DOUBLE(renut_memory_report, 30.0, "Nuts&Bolts/Debug",
                      "Seconds between memory reports in the log (process, GPU and the "
                      "renderer's pools); 0 turns them off.");

namespace renut::gpu {

namespace {

// Private: memory only this process has (what Task Manager's "Memory" column
// shows on Windows); resident: what of it, and of shared mappings, is in RAM.
struct ProcessMemory {
  bool valid = false;
  uint64_t private_bytes = 0;
  uint64_t resident_bytes = 0;
};

ProcessMemory QueryProcessMemory() {
  ProcessMemory memory;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                           sizeof(counters))) {
    memory.valid = true;
    memory.private_bytes = counters.PrivateUsage;
    memory.resident_bytes = counters.WorkingSetSize;
  }
#elif defined(__linux__)
  // RssAnon: resident anonymous memory, the closest to Windows' private bytes.
  if (FILE *status = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), status)) {
      unsigned long long kb = 0;
      if (std::sscanf(line, "RssAnon: %llu kB", &kb) == 1)
        memory.private_bytes = kb << 10;
      else if (std::sscanf(line, "VmRSS: %llu kB", &kb) == 1)
        memory.resident_bytes = kb << 10;
    }
    std::fclose(status);
    memory.valid = memory.resident_bytes != 0;
  }
#endif
  return memory;
}

double Mb(uint64_t bytes) {
  return double(bytes) / double(1u << 20);
}

} // namespace

#if defined(__SWITCH__)
extern "C" char **__system_argv;  // libnx: argv[0] is the NRO's path
extern "C" int __system_argc;

// tw2de (2026-10-26): <nro>.sym (build_switch.py: '<hex offset> <name>' lines, by offset), read at the first
// report, for the heap sites' frames (addresses as the code runs). Where the NRO was loaded ('base') comes from one
// function both know, HeapSitesEnable (libnx's __start__ read 0: the frames were not offsets), and a second one,
// HeapSitesReport, must then match too (a .sym of another build would name everything wrongly).
struct Symbols {
  bool tried = false, ok = false;
  uintptr_t base = 0;
  std::vector<uint32_t> offsets, names;  // names: where each name starts in 'text'
  std::string text, why;
};
Symbols &Syms() {
  static Symbols s;
  if (s.tried)
    return s;
  s.tried = true;
  if (__system_argc < 1 || !__system_argv || !__system_argv[0]) {
    s.why = "no NRO path";
    return s;
  }
  std::string path = __system_argv[0];
  const size_t dot = path.rfind('.');
  path = (dot == std::string::npos ? path : path.substr(0, dot)) + ".sym";
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    s.why = "no " + path;
    return s;
  }
  char line[512];
  uint32_t check = 0, check2 = 0;
  while (std::fgets(line, sizeof(line), f)) {
    char *end = nullptr;
    const unsigned long off = std::strtoul(line, &end, 16);
    if (!end || *end != ' ')
      continue;
    std::string name(end + 1);
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
      name.pop_back();
    if (name == "renut::nxs::HeapSitesEnable()")
      check = uint32_t(off);
    else if (name.rfind("renut::nxs::HeapSitesReport(", 0) == 0)
      check2 = uint32_t(off);
    s.offsets.push_back(uint32_t(off));
    s.names.push_back(uint32_t(s.text.size()));
    s.text += name;
    s.text += '\0';
  }
  std::fclose(f);
  const uintptr_t here = reinterpret_cast<uintptr_t>(&renut::nxs::HeapSitesEnable);
  const uintptr_t here2 = reinterpret_cast<uintptr_t>(&renut::nxs::HeapSitesReport);
  s.base = here - check;
  s.ok = check && check2 && here2 - s.base == check2;
  if (!s.ok)
    s.why = path + " is not this build's";
  RGPU_INFO("Memory: {} functions from {}{}; the NRO at {:X} (the sites' frames are addresses)", s.offsets.size(),
            path, s.ok ? "" : " (not this build's: unused)", here - check);
  return s;
}

std::string SymbolName(uintptr_t address) {
  const Symbols &s = Syms();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lX", static_cast<unsigned long>(address));
  if (!s.ok || address < s.base)
    return buf;
  const uintptr_t offset = address - s.base;
  auto it = std::upper_bound(s.offsets.begin(), s.offsets.end(), uint32_t(offset - 1));  // a return address
  if (it == s.offsets.begin())
    return buf;
  const size_t i = size_t(it - s.offsets.begin()) - 1;
  std::snprintf(buf, sizeof(buf), "+%lX", static_cast<unsigned long>(offset - s.offsets[i]));
  return std::string(&s.text[s.names[i]]) + buf;
}
#endif

unsigned long long HeapRoomBytes() {
#if defined(__SWITCH__)
  // tw2de (2026-11-05): the top chunk's size read directly, in place of mallinfo().keepcost. mallinfo takes the
  // malloc lock and walks every free chunk of all 128 bins (newlib's __malloc_update_mallinfo); with the game's heap
  // that took 13-20 ms (tw2de_diag_vsync_timing run 7ae93717: the cold-texture check every 32 frames, the idle one
  // every 120 and the export-buffer sweep every 300, each about 14 ms, all of it in this call), and every other
  // thread's malloc and free waited for it. keepcost is chunksize(top): __malloc_update_mallinfo loads
  // __malloc_av_[2] (top), its size word (offset 8) masked with ~3, and stores it as keepcost (disassembly of
  // libc_a-mallinfor.o, devkitA64). The same value, read under the same lock, in O(1).
  __malloc_lock(_REENT);
  const size_t *top = static_cast<const size_t *>(__malloc_av_[2]);
  const size_t keepcost = top ? top[1] & ~size_t(3) : 0;
  __malloc_unlock(_REENT);
  char *brk = static_cast<char *>(sbrk(0));
  return uint64_t(keepcost) + (fake_heap_end > brk ? uint64_t(fake_heap_end - brk) : 0);
#else
  return 0;
#endif
}

void MaybeReportMemory() {
  using Clock = std::chrono::steady_clock;
  const double interval = REXCVAR_GET(renut_memory_report);
  const Clock::time_point now = Clock::now();
  // tw2de (2026-10-26): the first report at the first frame, and one more whenever the heap's untouched top (what
  // sbrk can still give: one read, no walk of the heap) is 64 MB below the lowest reported, so one run shows the start
  // and every new peak, besides a report every renut_memory_report seconds. At most one every 2 s.
  static Clock::time_point last = now - std::chrono::hours(1);
  static uint64_t lowest = ~uint64_t(0);
  if (interval <= 0.0 || !REXCVAR_GET(log_diagnostics))
    return;
  uint64_t top = 0;
#if defined(__SWITCH__)
  {
    char *brk = static_cast<char *>(sbrk(0));
    top = fake_heap_end > brk ? uint64_t(fake_heap_end - brk) : 0;
  }
#endif
  const double since = std::chrono::duration<double>(now - last).count();
  const bool peak = top + (64ull << 20) < lowest && since >= 2.0;
  if (!peak && since < interval)
    return;
  last = now;
  lowest = std::min(lowest, top);

  // Outside State().mutex: the recording lock is never taken inside it.
  const CommandBufferMemory commands = CommandBufferMemoryUsage();

  auto &s = State();
  std::lock_guard lock(s.mutex);
  if (!s.host_ready)
    return;
  const ProcessMemory process = QueryProcessMemory();
  const GpuMemoryUsage gpu = QueryGpuMemory(s.device.get());
  size_t targets = 0;
  uint64_t target_bytes = 0;
  SurfaceMemoryLocked(targets, target_bytes);
  const TextureMemory textures = TextureMemoryLocked();
  const StreamCacheMemory streams = StreamCacheMemoryLocked();
  const PipelineCounts pipelines = PipelineCountsLocked();
  uint64_t uploads = 0, memexport = 0, readback = 0;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    uploads += s.uploads[i].Bytes();
    memexport += s.memexport_capacity[i] + s.memexport_scratch_capacity[i] +
                 s.exported_streams_capacity[i] + s.gpu_fetch_data_capacity[i];
    readback += s.memexport_readback_capacity[i];
  }

  RGPU_INFO("Memory: process {:.0f} MB private, {:.0f} MB resident; GPU {} (local {:.0f}/{:.0f} "
            "MB, shared {:.0f}/{:.0f} MB)",
            Mb(process.private_bytes), Mb(process.resident_bytes), gpu.valid ? "used/budget" : "n/a",
            Mb(gpu.local_used), Mb(gpu.local_budget), Mb(gpu.shared_used), Mb(gpu.shared_budget));
#if defined(__SWITCH__)
  // tw2de (2026-10-17): the Switch's malloc heap is fixed (__nx_heap_size, or the loader's), the guest's memory in
  // it: what the process line above cannot show there. GPU memory is not in it.
  {
    const struct mallinfo mi = mallinfo();
    const size_t heap = envHasHeapOverride() ? size_t(envGetHeapOverrideSize()) : __nx_heap_size;
    // tw2de (2026-10-26): what is left, and of it the room for a large block (HeapRoomBytes: the GPU budget's).
    RGPU_INFO("Memory: heap {:.0f} MB in use of {:.0f} MB, {:.0f} MB free ({:.0f} MB of it room for large blocks; "
              "{:.0f} MB taken from it so far, {:.0f} MB of that free in {} pieces)",
              Mb(mi.uordblks), Mb(heap), Mb(heap > mi.uordblks ? heap - mi.uordblks : 0), Mb(HeapRoomBytes()),
              Mb(mi.arena), Mb(mi.fordblks), size_t(mi.ordblks));
    // tw2de (2026-10-26): the heap's two big users: the game's memory and the GPU's (deko3d: its images, buffers,
    // shader code and command memory in use; its reserved image heap and buffer arena come on top).
#if defined(PLUME_DEKO3D)
    const plume::Deko3DStats &st = plume::GetDeko3DStats();
    if (IsDeko3D())
      RGPU_INFO("Memory: GPU {}", static_cast<plume::Deko3DDevice *>(s.device.get())->blockCensus());
    RGPU_INFO("Memory: heap users: guest memory {:.0f} MB in {} chunks; GPU {:.0f} MB in use (images {:.0f}, buffers "
              "{:.0f}, code {:.0f}, commands {:.0f})",
              Mb(RexGmCommittedBytes()), RexGmChunkCount(),
              Mb(st.imageBytes + st.bufferBytes + st.codeBytes + st.commandBytes), Mb(st.imageBytes),
              Mb(st.bufferBytes), Mb(st.codeBytes), Mb(st.commandBytes));
#endif
    // tw2de (2026-10-26): the guest threads' host stacks (xthread.cpp kGuestHostStackBytes each): how deep each
    // went so far, deepest first, for choosing that size.
    {
      size_t used[128], sizes[128];
      const size_t n = RexSwitchStackDepths(used, sizes, 128);
      if (n) {
        std::sort(used, used + n, std::greater<size_t>());
        uint64_t total = 0;
        for (size_t i = 0; i < n; ++i)
          total += sizes[i];
        std::string depths;
        for (size_t i = 0; i < n; ++i)
          depths += (i ? ", " : "") + std::to_string((used[i] + 1023) >> 10);
        RGPU_INFO("Memory: host stacks: {} guest threads, {:.0f} MB in all; deepest reached (KB): {}", n, Mb(total),
                  depths);
      }
    }
    // tw2de (2026-10-26): every live heap block of 256 KB or more by where it was allocated (its stack's first
    // frames, named from <nro>.sym), and what is left: the small blocks.
    {
      unsigned n = 0;
      const uint64_t big = renut::nxs::HeapSitesReport(
          [](uint64_t bytes, uint32_t count, const uintptr_t *frames, void *arg) {
            std::string where;
            for (unsigned i = 0; i < renut::nxs::kHeapSiteFrames; ++i)
              if (frames[i])
                where += (where.empty() ? "" : " <- ") + SymbolName(frames[i]);
            RGPU_INFO("Memory: heap site {}: {:.1f} MB in {} blocks: {}", ++*static_cast<unsigned *>(arg),
                      Mb(bytes), count, where);
          },
          &n, 40);
      RGPU_INFO("Memory: heap blocks of 256 KB and more {:.0f} MB (made since the start; the sites above), the rest "
                "{:.0f} MB (smaller blocks)",
                Mb(big), Mb(mi.uordblks > big ? mi.uordblks - big : 0));
    }
  }
#endif
  RGPU_INFO("Memory: GPU: {} render targets {:.0f} MB (x{:.3g} scale), {} textures {:.0f} MB (of "
            "which {} resolve targets {:.0f} MB), memexport {:.0f} MB",
            targets, Mb(target_bytes), ResolutionScale().Factor(), textures.textures, Mb(textures.bytes),
            textures.resolve_targets, Mb(textures.resolve_bytes), Mb(memexport));
  RGPU_INFO("Memory: host: upload rings {:.0f} MB, stream cache {:.0f} MB in a {:.0f} MB pool "
            "({} streams, {} index ranges), memexport readback {:.0f} MB, command buffers "
            "{:.0f} MB in {} ({} live) + {:.0f} MB in {} spare, shader cache {:.0f} MB, "
            "pipelines {} built + {} prebuilt unused (of {} listed, {} waiting for their shaders)",
            Mb(uploads), Mb(streams.cached_bytes), Mb(streams.pool_bytes), streams.streams,
            streams.index_ranges, Mb(readback), Mb(commands.recorded_bytes), commands.recorded,
            commands.live, Mb(commands.spare_bytes), commands.spare, Mb(ShaderCacheHostBytes()),
            pipelines.built, pipelines.prebuilt_unused, pipelines.listed, pipelines.waiting);
}

} // namespace renut::gpu

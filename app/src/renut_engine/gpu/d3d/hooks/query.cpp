// TW2 (2026-11-12): the game's occlusion queries, counted by the host GPU.
//
// TW2's D3D (XDK 2.0.21076) issues a query with D3DQuery_Issue (sub_82D110B0: r3 the query, r4 the flags,
// 2 begin, 1 end) and reads it with D3DQuery_GetData (sub_82D11360). An occlusion query is type 9 ([query +4]); its
// result memory is one 64-byte block per tile ([query +0x1C + 4i], physical addresses, i < [query +0x94]). At its end
// the 360 GPU first writes 0xFFFFFEED into each block's +0x10 and +0x14 (MEM_WRITE packets in the ring), then the
// sample counts of the ZPASS_DONE events; GetData waits for the query's fence ([query +0x18]), then for those two
// words to change, and sums each block's little-endian words: (+0x10 + +0x14) - (+0x30 + +0x34) samples, with more
// statistics in the other words when the caller asks for 16 bytes.
//
// Renut draws with deko3d and executes none of the ring's packets, so the blocks were never written (the first
// new-game load waited for ever). Here, at each begin and end, a deko3d samples-passed report is recorded into the
// render thread's stream between the same draws (the D3D draws reach it in order), into a query pool of the host GPU;
// at the end the blocks get the sentinel at once (as the GPU's MEM_WRITE would, before the counts), and once the GPU
// has written both reports (their timestamps are no longer 0), the samples that passed between them are written into
// the first block as the 360 GPU's sums would give them (every other word 0). GetData itself is the game's: it is
// only preceded by that check, so its fence wait, its S_FALSE and its result are its own.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <unordered_map>
#include <vector>
#include <thread>
#include <chrono>

#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/lookahead.h"  // TW2 (2026-11-14, tw2de_lookahead)
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/tw2de_renut_select.h"
#include <rex/types.h>
#include <plume_deko3d.h>

REXCVAR_DECLARE(bool, log_diagnostics);
REXCVAR_DEFINE_BOOL(tw2de_diag_queries, false, "Game/Diagnostics",
                    "TW2 diagnostic (2026-11-12, flares through objects): a log line per finished occlusion query (its "
                    "samples) and its begin and end in a frame dump's draws.txt");
namespace renut::gpu {
void FrameDumpNoteLocked(const std::string &line);  // render/draw.cpp
// TW2 (2026-11-12): occlusion queries begun and not yet ended in the render thread's stream (render thread only):
// render/draw.cpp keeps every draw while one is open (an inert draw is what a query counts).
int g_tw2_open_queries = 0;
}

using namespace renut::gpu;

REX_EXTERN(__imp__sub_82D110B0);  // D3DQuery_Issue(query, flags)
REX_EXTERN(__imp__sub_82D11360);  // D3DQuery_GetData(query, data, size, flags)

namespace {
constexpr u32 kOcclusionQuery = 9;
constexpr u32 kIssueEnd = 1, kIssueBegin = 2;
constexpr u32 kSentinel = 0xFFFFFEEDu;
constexpr uint32_t kReports = 4096;  // a ring of the pool's reports

struct Pending {
  u32 query;
  std::vector<u32> blocks;  // the result blocks' virtual addresses
  int32_t begin, end;       // report indices (begin -1: none recorded)
};

std::mutex g_mutex;                         // the pending list and the begin map
std::vector<Pending> g_pending;
std::unordered_map<u32, int32_t> g_begin;   // query -> its begin report (render thread)
std::unique_ptr<plume::RenderQueryPool> g_pool;  // render thread (created there)
const volatile uint64_t *g_reports = nullptr;
uint32_t g_next = 0;
std::atomic<uint64_t> g_resolved{0};

u32 Be32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

// A physical address as the 360's D3D turns it into a virtual one (GetData: 0xC0000000 + the address, plus 4 KB
// from 0x1FE00000 up).
u32 BlockVa(u32 phys) { return 0xC0000000u + (phys & 0x1FFFFFFFu) + (((phys >> 20) + 0x200u) & 0x1000u); }

std::vector<u32> Blocks(u32 query) {
  std::vector<u32> out;
  const u32 tiles = Be32(query + 0x94);
  for (u32 i = 0; i < tiles && i < 16; i++)
    if (const u32 phys = Be32(query + 0x1C + i * 4))
      out.push_back(BlockVa(phys));
  return out;
}

// Render thread, State().mutex held: a samples-passed report into the next slot of the ring.
int32_t ReportLocked() {
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd && Video::EnsureCommandListLocked())
    cmd = Video::CommandListLocked();
  if (!cmd)
    return -1;
  if (!g_pool) {
    if (plume::RenderDevice *device = Video::Device())
      g_pool = device->createQueryPool(kReports);
    g_reports = g_pool ? plume::Deko3DQueryReports(g_pool.get()) : nullptr;
    if (!g_reports) {
      RGPU_ERROR("[queries] no query pool for the occlusion queries");
      g_pool.reset();
      return -1;
    }
  }
  const uint32_t index = g_next;
  g_next = (g_next + 1) % kReports;
  const_cast<volatile uint64_t *>(g_reports)[index * 2] = 0;
  const_cast<volatile uint64_t *>(g_reports)[index * 2 + 1] = 0;  // the timestamp: 0 until the GPU writes it
  cmd->writeSamplesPassed(g_pool.get(), index);
  return int32_t(index);
}

bool Written(int32_t index) { return index >= 0 && g_reports && g_reports[uint32_t(index) * 2 + 1] != 0; }

// The finished queries' results into their blocks (any thread).
void Resolve() {
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < g_pending.size();) {
    Pending &p = g_pending[i];
    if (p.end < 0 || !Written(p.end) || (p.begin >= 0 && !Written(p.begin))) {
      ++i;
      continue;
    }
    const uint64_t end = g_reports[uint32_t(p.end) * 2];
    const uint64_t begin = p.begin >= 0 ? g_reports[uint32_t(p.begin) * 2] : end;
    const uint32_t samples = uint32_t(end >= begin ? std::min<uint64_t>(end - begin, 0xFFFFFFFFu) : 0);
    for (size_t b = 0; b < p.blocks.size(); b++) {
      if (u8 *block = GuestPtr<u8>(p.blocks[b])) {
        std::memset(block, 0, 0x40);
        if (b == 0)
          std::memcpy(block + 0x10, &samples, 4);  // little-endian, as GetData reads it
      }
    }
    if (REXCVAR_GET(tw2de_diag_queries)) {
      static std::atomic<uint32_t> lines{0};
      if (lines.fetch_add(1) < 3000)
        RGPU_INFO("[queries] {:08X}: {} samples ({} blocks; reports {} {})", p.query, samples, p.blocks.size(), p.begin,
                  p.end);
    }
    g_resolved.fetch_add(1, std::memory_order_relaxed);
    p = std::move(g_pending.back());
    g_pending.pop_back();
  }
}
}  // namespace

extern "C" REX_FUNC(sub_82D110B0) {
  const u32 query = ctx.r3.u32, flags = ctx.r4.u32;
  __imp__sub_82D110B0(ctx, base);
  if (!tw2de::renut_gpu::Active() || !query || Be32(query + 4) != kOcclusionQuery || !(flags & (kIssueBegin | kIssueEnd)))
    return;
  const u32 device = Be32(query);
  if (device)
    FlushDeferredVertices(device);  // the draws before it are queued first
  if (flags & kIssueBegin) {
    // TW2 (2026-11-14, tw2de_lookahead): a query's begin and end sample nothing and write no destination; untagged
    // they stopped the render thread's look ahead (0.4 a frame: the occlusion boxes come between a depth-writing
    // draw and the depth resolve that makes the earlier one dead).
    render_thread::QueueTagged(&lookahead::kNoneTag, nullptr, nullptr, false, [query] {
      if (REXCVAR_GET(tw2de_diag_queries))
        FrameDumpNoteLocked(fmt::format("query {:08X} begin", query));
      const int32_t index = ReportLocked();
      std::lock_guard lock(g_mutex);
      if (!g_begin.count(query))
        ++g_tw2_open_queries;
      g_begin[query] = index;
    });
  }
  if (flags & kIssueEnd) {
    std::vector<u32> blocks = Blocks(query);
    {
      // The 360 GPU's MEM_WRITE before the counts: GetData answers S_FALSE until the result is written. An older
      // result of the same query still on its way would overwrite this one: dropped.
      std::lock_guard lock(g_mutex);
      for (size_t i = 0; i < g_pending.size();)
        if (g_pending[i].query == query) {
          g_pending[i] = std::move(g_pending.back());
          g_pending.pop_back();
        } else {
          ++i;
        }
      for (const u32 va : blocks)
        if (auto *w = GuestPtr<be_u32>(va)) {
          w[4] = kSentinel;
          w[5] = kSentinel;
        }
    }
    render_thread::QueueTagged(&lookahead::kNoneTag, nullptr, nullptr, false,
                               [query, blocks = std::move(blocks)]() mutable {
      if (REXCVAR_GET(tw2de_diag_queries))
        FrameDumpNoteLocked(fmt::format("query {:08X} end", query));
      const int32_t index = ReportLocked();
      std::lock_guard lock(g_mutex);
      int32_t begin = -1;
      if (auto it = g_begin.find(query); it != g_begin.end()) {
        --g_tw2_open_queries;
        begin = it->second;
        g_begin.erase(it);
      }
      g_pending.push_back({query, std::move(blocks), begin, index});
    });
  }
  static std::atomic<bool> logged{false};
  if (REXCVAR_GET(log_diagnostics) && !logged.exchange(true))
    RGPU_INFO("[queries] the game's first occlusion query: counted by the host GPU (samples passed)");
}

// TW2 (2026-11-12): the game's only caller of GetData (sub_82E0A8A8) waits for a result by calling it again at once
// while it answers S_FALSE (1): the render thread spun on the host GPU's report (GetData's own fence check, Resolve,
// its block loop with dcbf: 4.5 % of the thread in the prologue, besides the time). From that loop (lr 0x82E0A90C)
// a S_FALSE yields the core first, and after 16 in a row sleeps 50 us a turn (the feed thread and the GPU that will
// answer are elsewhere). The same calls, the same results: only the time between two of them.
REXCVAR_DEFINE_BOOL(tw2de_query_wait_yield, true, "Game/Enhancements",
                    "TW2: the game's wait for an occlusion query result (it calls D3DQuery_GetData again at once while "
                    "the answer is S_FALSE) yields the core between calls, then sleeps 50 us a turn")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" REX_FUNC(sub_82D11360) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (tw2de::renut_gpu::Active())
    Resolve();
  __imp__sub_82D11360(ctx, base);
  if (lr != 0x82E0A90Cu || !REXCVAR_GET(tw2de_query_wait_yield))
    return;
  static thread_local uint32_t s_false_in_row = 0;
  // TW2 (2026-11-12, with log_diagnostics): how many waits and how long, every 10 s ([queries] wait line).
  static thread_local std::chrono::steady_clock::time_point s_wait_start, s_report;
  static thread_local uint64_t s_calls = 0, s_waits = 0, s_wait_us = 0;
  if (ctx.r3.u32 != 1) {
    if (REXCVAR_GET(log_diagnostics)) {
      const auto now = std::chrono::steady_clock::now();
      ++s_calls;
      if (s_false_in_row) {
        ++s_waits;
        s_wait_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - s_wait_start).count());
      }
      if (now - s_report > std::chrono::seconds(10)) {
        if (s_report.time_since_epoch().count())
          RGPU_INFO("[queries] 10 s: {} results read, {} of them waited for, {:.1f} ms waiting in all", s_calls, s_waits,
                    double(s_wait_us) / 1000.0);
        s_report = now;
        s_calls = s_waits = s_wait_us = 0;
      }
    }
    s_false_in_row = 0;
    return;
  }
  if (s_false_in_row == 0)
    s_wait_start = std::chrono::steady_clock::now();
  if (++s_false_in_row <= 16)
    std::this_thread::yield();
  else
    std::this_thread::sleep_for(std::chrono::microseconds(50));
}

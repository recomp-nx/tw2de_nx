/**
 * @file    gpu/backend/render_thread.cpp
 */
#include "renut_engine/gpu/render/draw.h"  // tw2de (2026-11-05): rtw ([rtwork])
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/lookahead.h"  // TW2 (2026-11-14, tw2de_lookahead)

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <fmt/format.h>
#include <string>
#include <rex/frame_latency.h>
#include <vector>

#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"
#if defined(__SWITCH__)
#include "profiler/switch_cpu_profiler.h"  // tw2de (2026-10-24): MarkSlowFrameCompanion
#endif

#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

// tw2de (2026-10-20, from sa2-nx): see g_done_waiters and Run.
REXCVAR_DEFINE_BOOL(tw2de_renut_done_notify_waiters, true, "Game/Enhancements",
                    "Renut's render thread signals its work done (frames, fences, drains) only when a thread waits for "
                    "it: a signal is a kernel call on the Switch even with nobody waiting, and it made one after every "
                    "batch of work. Off: after every batch, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_renut_prefetch_next, true, "Game/Enhancements",
                    "Renut's render thread asks for the next draw's capture (written on the game thread's core) to be "
                    "brought into its cache while it does the draw before it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_renut_wake_batch, 16, "Game/Enhancements",
                     "Renut's render thread, when it has caught up and sleeps, is woken by the game's draws only once "
                     "this many are queued (each wake is a kernel call on both threads; the fourth console run woke it "
                     "for almost every draw). Every other item (frames, fences, resources), every wait for it and a "
                     "2 ms timer still wake it at once. 1 or less: every draw wakes it, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// TW2 (2026-11-12, from renut_nx_ultimate_base port 105: dishonored-nx's, sa2-nx's of 2026-10-28).
REXCVAR_DEFINE_BOOL(tw2de_renut_lockfree_queue, true, "Game/Enhancements",
                    "Renut's render thread takes its work from a lock-free ring (one compare-and-swap a push) instead "
                    "of a vector under the queue's mutex, which every draw of the game thread took and the render "
                    "thread took back (two lock round trips a draw on two cores, and the wake decision under the lock). "
                    "Same order, same wakes. Off: the mutex and the vector")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tw2de_frames_ahead, 1, "Game/Enhancements",
                     "Frames the game may queue ahead of Renut's render thread before its Swap waits (1 as before; 2: "
                     "more overlap between the game's threads and the render thread, a frame more of input latency)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_renut_batch_clock, true, "Game/Enhancements",
                    "Renut's render thread reads the clock once per batch of work instead of once per item (about "
                    "1,800 items a frame in City Escape); its busy time per frame is the same sum")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace renut::gpu {
bool StepDeferredMemExportsIdle();  // tw2de (2026-10-28): render/draw.cpp
}

namespace renut::gpu::render_thread {

std::atomic<bool> g_idle_work{false};  // tw2de (2026-10-28): RequestIdleWork

namespace {

using Clock = std::chrono::steady_clock;

constexpr int32_t kNoTag = INT32_MIN;
struct Item {
  std::function<void()> work;
  bool locked = true;  // run with State().mutex held
  // TW2 (2026-11-14, tw2de_lookahead): its lookahead::Tag at prefetch[0] + tag_offset (in the padding: the ring's
  // slots stay one cache line).
  int32_t tag_offset = kNoTag;
  const void *prefetch[2] = {};  // tw2de (2026-10-20, from sa2-nx, tw2de_renut_prefetch_next): fetched while the item before runs
};

static_assert(sizeof(Item) <= sizeof(std::function<void()>) + 24, "the tag offset sits in Item's padding");

// tw2de (2026-10-20, from sa2-nx, tw2de_renut_prefetch_next): the next item's data into the cache (its first lines).
inline void PrefetchItem(const Item &item) {
  if (const auto *a = static_cast<const char *>(item.prefetch[0])) {
    __builtin_prefetch(a);
    __builtin_prefetch(a + 64);
  }
  if (const auto *b = static_cast<const char *>(item.prefetch[1])) {
    __builtin_prefetch(b);
    __builtin_prefetch(b + 64);
    __builtin_prefetch(b + 128);
    __builtin_prefetch(b + 192);
  }
}

std::mutex g_mutex;
std::condition_variable g_work_cv;  // work queued, or stopping
// tw2de (tw2de_renut_queue_notify_idle): the render thread is in g_work_cv.wait (under g_mutex). A push notifies only
// then: on the Switch every notify is a kernel call, about 1,500 a frame from the game threads, most of them while the
// render thread is busy and not waiting. The render thread reads the queue under g_mutex before it sleeps, so a push
// it has not seen is never missed.
bool g_render_waiting = false;
std::condition_variable g_done_cv;  // work done: frames, fences, drains; or stopped
// tw2de (2026-10-20, from sa2-nx, tw2de_renut_done_notify_waiters): threads in g_done_cv's waits (under g_mutex). A done signal goes
// only when there is one: a waiter checks its condition under g_mutex before it counts itself and sleeps, and the
// state it waits for changes under g_mutex too, so no signal it needs is skipped.
int g_done_waiters = 0;
bool DoneNotifyAll() {
  static const bool on = REXCVAR_GET(tw2de_renut_done_notify_waiters);
  return !on;
}
// tw2de (2026-10-20, from sa2-nx, tw2de_renut_wake_batch): the draws queued before a sleeping render thread is woken (1: each).
size_t WakeBatch() {
  static const size_t n = size_t(std::max(1, int(REXCVAR_GET(tw2de_renut_wake_batch))));
  return n;
}
std::vector<Item> g_queue;

// TW2 (2026-11-12, renut_nx_ultimate_base port 105, tw2de_renut_lockfree_queue; dishonored-nx's, sa2-nx's of
// 2026-10-28): the queue as a bounded ring of kRingSlots items (D. Vyukov's bounded MPMC queue, with the render thread
// its one consumer). A push claims the next position with a compare-and-swap, moves its item into the slot and
// publishes it by the slot's sequence (release); the render thread takes the positions in order (acquire), runs each
// item in its slot and gives the slot back for the push kRingSlots positions later. The order is the order of the
// claims, as it was the order of the lock. A full ring makes a pusher wait for the render thread; the render thread
// itself runs its own item at once instead (logged: never expected). sa2-nx's console profile: the queue's locks
// 0.83 -> 0.18 ms a frame. Latched by Start before the thread is made and before g_started's release.
bool g_lockfree = false;
bool LockfreeOn() {
  return g_lockfree;
}
bool g_notify_idle = true;
size_t g_wake_batch = 16;
// sa2-nx: 16,384 slots (1 MB) and the slot kRingAhead positions on prefetched for writing by a push (32,768 without the
// prefetch was slower: every push's slot a line from memory).
constexpr uint64_t kRingSlots = uint64_t(1) << 14;
constexpr uint64_t kRingAhead = 8;
struct alignas(64) RingSlot {
  std::atomic<uint64_t> seq{0};
  Item item;
};
RingSlot *g_ring = nullptr;                      // made by Start, before the thread runs
alignas(64) std::atomic<uint64_t> g_enqueue{0};  // the next position a push claims (the items queued)
alignas(64) std::atomic<uint64_t> g_dequeue{0};  // the next position the render thread takes (its own)
std::atomic<bool> g_waiting{false};              // the render thread is going to sleep, or asleep, in g_work_cv
std::atomic<bool> g_accepting{false};            // started and not stopping: pushes go to the ring
inline bool RingReady(uint64_t pos) {
  return g_ring[pos & (kRingSlots - 1)].seq.load(std::memory_order_acquire) == pos + 1;
}
// The sleeping side's last look: sequentially consistent, ordered after its store of g_waiting.
inline bool RingReadySc(uint64_t pos) {
  return g_ring[pos & (kRingSlots - 1)].seq.load(std::memory_order_seq_cst) == pos + 1;
}
// bangaio-nx (via the base): set once Start's call_once has run; Push checks it first instead of std::call_once (a
// pthread_once call at every queued draw).
std::atomic<bool> g_started{false};
std::once_flag g_start_once;
std::thread::id g_thread_id;
bool g_running = false;   // the thread takes work
bool g_stopping = false;  // Stop: finish what is queued, then end
uint64_t g_items_queued = 0;
uint64_t g_items_done = 0;
uint64_t g_frames_queued = 0;
uint64_t g_frames_done = 0;
uint64_t g_fences_done = 0;  // fence completions written, plus callbacks reached
uint64_t g_callbacks_reached = 0;
uint64_t g_callbacks_run = 0;
// Fence completions waiting for their callbacks, in order.
struct PendingFence {
  u32 completion_va;
  u32 fence;
  uint64_t after_callback;
};
std::vector<PendingFence> g_pending_fences;

// Requires g_mutex.
void WriteFence(u32 completion_va, u32 fence) {
  if (auto *word = GuestPtr<be_u32>(completion_va))
    *word = fence;
  ++g_fences_done;
}

// Render thread only: time spent running work since the last frame ended, and
// when the running item started; of that time, how much it waited (WaitScope).
Clock::duration g_busy{};
Clock::time_point g_item_start{};
Clock::duration g_waited{};
thread_local bool t_render_thread = false;

// TW2 (2026-11-13, tw2de_repeat_draw_fast): counts the items run, so a draw can tell that nothing else ran since the
// draw before it (ItemSerial).
uint64_t g_item_serial = 0;

// TW2 (2026-11-14, tw2de_lookahead): the ring position and tag of the item RunRing is running.
uint64_t g_exec_pos = UINT64_MAX;
const void *g_exec_tag = nullptr;
inline const void *ItemTag(const Item &item) {
  return item.tag_offset == kNoTag ? nullptr : static_cast<const char *>(item.prefetch[0]) + item.tag_offset;
}

void Execute(Item &item) {
  ++g_item_serial;
  if (item.locked) {
    // tw2de (2026-11-05, [rtwork]): the wait for State().mutex (a game thread's hook holding it) timed.
    std::unique_lock lock(State().mutex, std::defer_lock);
    if (rtw::On()) {
      const rtw::Scope wait(rtw::kLockWait);
      lock.lock();
    } else {
      lock.lock();
    }
    item.work();
  } else {
    item.work();
  }
}

// TW2 (2026-11-12, port 105, tw2de_renut_lockfree_queue): Run with the ring. The items are taken in order as long as
// they are published, at most kRingRun before the done bookkeeping; with none ready the thread does its idle work
// (RequestIdleWork), then says it is going to sleep (g_waiting), looks once more, and sleeps in g_work_cv under g_mutex:
// a push that read g_waiting false after publishing (both sides store, then load, sequentially consistent) is seen by
// that last look, and one that read it true takes g_mutex before it notifies, so it cannot notify between the look and
// the wait.
constexpr size_t kRingRun = 256;
void RunRing() {
  static const bool batch_clock = REXCVAR_GET(tw2de_renut_batch_clock);
  static const bool prefetch = REXCVAR_GET(tw2de_renut_prefetch_next);
  uint64_t pos = g_dequeue.load(std::memory_order_relaxed);
  for (;;) {
    if (!RingReady(pos)) {
      // tw2de (2026-10-28): idle work while nothing is queued, a part at a time.
      while (!RingReady(pos) && g_idle_work.load(std::memory_order_relaxed) &&
             g_accepting.load(std::memory_order_relaxed)) {
        if (!StepDeferredMemExportsIdle())
          g_idle_work.store(false, std::memory_order_relaxed);
      }
    }
    if (!RingReady(pos)) {
      g_waiting.store(true, std::memory_order_seq_cst);
      bool stopping = false;
      if (!RingReadySc(pos)) {
        std::unique_lock lock(g_mutex);
        g_render_waiting = true;
        if (g_wake_batch > 1) {
          // draws queued without a wake are taken within 2 ms at the latest.
          while (!RingReady(pos) && !g_stopping)
            g_work_cv.wait_for(lock, std::chrono::milliseconds(2));
        } else {
          g_work_cv.wait(lock, [pos] { return RingReady(pos) || g_stopping; });
        }
        g_render_waiting = false;
        stopping = g_stopping;
      }
      g_waiting.store(false, std::memory_order_relaxed);
      if (!RingReady(pos)) {
        if (stopping)
          break;
        continue;
      }
    }
    const Clock::time_point start = Clock::now();
    if (batch_clock)
      g_item_start = start;
    size_t n = 0;
    Clock::time_point item_start = start;
    do {
      RingSlot &slot = g_ring[pos & (kRingSlots - 1)];
      if (prefetch && RingReady(pos + 1))
        PrefetchItem(g_ring[(pos + 1) & (kRingSlots - 1)].item);
      if (!batch_clock)
        g_item_start = item_start;
      g_exec_pos = pos;  // TW2 (2026-11-14, tw2de_lookahead)
      g_exec_tag = ItemTag(slot.item);
      Execute(slot.item);
      g_exec_tag = nullptr;
      g_exec_pos = UINT64_MAX;
      slot.item.work = nullptr;  // the closure's captures go now, as the batch's cleared vector freed them
      slot.seq.store(pos + kRingSlots, std::memory_order_release);
      ++pos;
      g_dequeue.store(pos, std::memory_order_relaxed);
      ++n;
      if (!batch_clock) {
        const Clock::time_point end = Clock::now();
        g_busy += end - item_start;
        item_start = end;
      }
    } while (n < kRingRun && RingReady(pos));
    if (batch_clock)
      g_busy += Clock::now() - start;
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_items_done += n;
      wake = g_done_waiters > 0 || DoneNotifyAll();
    }
    if (wake)
      g_done_cv.notify_all();
  }
  {
    std::lock_guard lock(g_mutex);
    g_running = false;
  }
  g_accepting.store(false, std::memory_order_release);
  g_done_cv.notify_all();
}

void Run() {
  {
    // tw2de: the thread may be a plain pthread on the Switch (Start), so it records its own id.
    std::lock_guard lock(g_mutex);
    g_thread_id = std::this_thread::get_id();
  }
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"renut render");
#endif
  tw2de::renut_gpu::OnRenderThreadStart();  // tw2de: its core and priority on the Switch
#if defined(__SWITCH__)
  // tw2de (2026-10-24): sampled through the game thread's slow frames too (tw2de_cpu_profiler,
  // tw2de_slow_frame_profile_ms): what the render thread did while the game waited for its fences.
  os::switch_cpu_profiler::MarkSlowFrameCompanion("render");
#endif
  t_render_thread = true;
  if (LockfreeOn()) {  // TW2 (2026-11-12, port 105, tw2de_renut_lockfree_queue)
    RunRing();
    return;
  }
  std::vector<Item> batch;
  for (;;) {
    {
      std::unique_lock lock(g_mutex);
      // tw2de (2026-10-28): idle work (RequestIdleWork) while nothing is queued, a part at a time, without g_mutex.
      while (g_queue.empty() && !g_stopping && g_idle_work.load(std::memory_order_relaxed)) {
        lock.unlock();
        const bool more = StepDeferredMemExportsIdle();
        if (!more)
          g_idle_work.store(false, std::memory_order_relaxed);
        lock.lock();
      }
      g_render_waiting = true;
      if (WakeBatch() > 1) {
        // tw2de (from sa2-nx) (tw2de_renut_wake_batch): draws queued without a wake are taken within 2 ms at the latest.
        while (g_queue.empty() && !g_stopping)
          g_work_cv.wait_for(lock, std::chrono::milliseconds(2));
      } else {
        g_work_cv.wait(lock, [] { return !g_queue.empty() || g_stopping; });
      }
      g_render_waiting = false;
      if (g_queue.empty())
        break;
      batch.swap(g_queue);
    }
    // tw2de (2026-10-19, render-thread proposal 19): one clock read per item (the end of one is the start of the
    // next). Its other half, one State().mutex hold across consecutive locked items, hung the emulator run at the
    // first loading screen (a game thread's hook waits on the lock while the render thread's next item waits on it):
    // each item takes the lock on its own, as before.
    Clock::time_point start = Clock::now();
    static const bool batch_clock = REXCVAR_GET(tw2de_renut_batch_clock);
    if (batch_clock) {
      // tw2de (2026-10-20, from sa2-nx): the batch's start for every item (QueueFrame's item counts its frame up to itself from
      // there and leaves the rest of the batch to the next frame), one clock read at the end.
      g_item_start = start;
      static const bool prefetch = REXCVAR_GET(tw2de_renut_prefetch_next);
      const size_t n = batch.size();
      for (size_t i = 0; i < n; ++i) {
        if (prefetch && i + 1 < n)
          PrefetchItem(batch[i + 1]);
        Execute(batch[i]);
      }
      g_busy += Clock::now() - start;
    } else {
      for (Item &item : batch) {
        g_item_start = start;
        Execute(item);
        const Clock::time_point end = Clock::now();
        g_busy += end - start;
        start = end;
      }
    }
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_items_done += batch.size();
      wake = g_done_waiters > 0 || DoneNotifyAll();
    }
    if (wake)
      g_done_cv.notify_all();
    batch.clear();
  }
  {
    std::lock_guard lock(g_mutex);
    g_running = false;
  }
  g_done_cv.notify_all();
}

void Start() {
  std::call_once(g_start_once, [] {
    InstallLateInput();  // tw2de (2026-10-19, tw2de_late_input): the delay function (it reads the key per frame)
    std::lock_guard lock(g_mutex);
    g_running = true;
    // TW2 (port 105, tw2de_renut_lockfree_queue): latched before the thread is made and before g_started's release;
    // the ring, each slot free for its position.
    g_lockfree = REXCVAR_GET(tw2de_renut_lockfree_queue);
    g_notify_idle = tw2de::renut_gpu::QueueNotifyIdleOn();
    g_wake_batch = WakeBatch();
    if (LockfreeOn()) {
      g_ring = new RingSlot[kRingSlots];
      for (uint64_t i = 0; i < kRingSlots; ++i)
        g_ring[i].seq.store(i, std::memory_order_relaxed);
      g_accepting.store(true, std::memory_order_release);
    }
    // Never joined or destroyed: the renderer's objects are left at exit too
    // (Video::BeginShutdown), and a joinable std::thread destroyed at exit
    // would terminate the process.
    // tw2de: a 1 MB stack on the Switch (the conversions and the driver run deep on this thread), never joined
    // (tw2de_renut_tuning.h); Run records its id.
    tw2de::renut_gpu::StartLongLivedThread(Run, 1024 * 1024);
    if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("Render thread started{}", LockfreeOn() ? " (lock-free queue)" : "");
  });
  // TW2 (port 105): release, read with acquire by Push: the ring's latch and the ring itself are seen with it.
  g_started.store(true, std::memory_order_release);
}

// tw2de (2026-10-20, from sa2-nx, tw2de_renut_wake_batch): before a thread waits for the render thread, the draws queued without a
// wake are handed over. Requires g_mutex.
void WakeForWaitLocked() {
  if (LockfreeOn()) {  // TW2 (port 105, tw2de_renut_lockfree_queue)
    if (g_render_waiting && g_ring && RingReady(g_dequeue.load(std::memory_order_relaxed)))
      g_work_cv.notify_one();
    return;
  }
  if (g_render_waiting && !g_queue.empty())
    g_work_cv.notify_one();
}

// TW2 (2026-11-12, port 105, tw2de_renut_lockfree_queue): Push with the ring; false when the item was not queued (the
// ring is full and this is the render thread, which cannot wait for itself). A non-draw item, or g_wake_batch draws not
// yet taken, wakes a render thread that is going to sleep (RunRing says why no wake is lost).
bool PushRing(Item &&item, bool draw) {
  uint64_t pos = g_enqueue.load(std::memory_order_relaxed);
  RingSlot *slot;
  for (;;) {
    slot = &g_ring[pos & (kRingSlots - 1)];
    const uint64_t seq = slot->seq.load(std::memory_order_acquire);
    const int64_t diff = int64_t(seq - pos);
    if (diff == 0) {
      if (g_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
        break;
    } else if (diff < 0) {
      // Full: the render thread is kRingSlots items behind.
      if (t_render_thread) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
          RGPU_WARN("Render thread queue full ({} items): the render thread's own item ran at once "
                    "(tw2de_renut_lockfree_queue)", kRingSlots);
        return false;
      }
      if (g_waiting.load(std::memory_order_seq_cst)) {  // (a signal is a kernel call on the Switch: only to a sleeper)
        {
          std::lock_guard lock(g_mutex);
        }
        g_work_cv.notify_one();
      }
      std::this_thread::yield();
      pos = g_enqueue.load(std::memory_order_relaxed);
    } else {
      pos = g_enqueue.load(std::memory_order_relaxed);
    }
  }
  __builtin_prefetch(&g_ring[(pos + kRingAhead) & (kRingSlots - 1)], 1);
  slot->item = std::move(item);
  // Published, then g_waiting read: a sequentially consistent store and load (stlr, ldar), RunRing's order.
  slot->seq.store(pos + 1, std::memory_order_seq_cst);
  const bool wake = (g_waiting.load(std::memory_order_seq_cst) &&
                     (!draw || pos + 1 - g_dequeue.load(std::memory_order_relaxed) >= g_wake_batch)) ||
                    !g_notify_idle;
  if (wake) {
    {
      std::lock_guard lock(g_mutex);
    }
    g_work_cv.notify_one();
  }
  return true;
}

void Push(std::function<void()> work, bool locked, const void *prefetch0 = nullptr,
          const void *prefetch1 = nullptr, bool draw = false, int32_t tag_offset = kNoTag) {
  if (!g_started.load(std::memory_order_acquire)) [[unlikely]]
    Start();
  if (LockfreeOn()) {  // TW2 (port 105, tw2de_renut_lockfree_queue)
    Item item{std::move(work), locked, tag_offset, {prefetch0, prefetch1}};
    if (g_accepting.load(std::memory_order_acquire) && PushRing(std::move(item), draw))
      return;
    // Stopping, or the render thread's own item in a full ring: run here, as a push while stopping did.
    Execute(item);
    return;
  }
  {
    std::unique_lock lock(g_mutex);
    // (Work queued by the render thread's own work runs after it.)
    if (g_running && !g_stopping) {
      g_queue.push_back({std::move(work), locked, tag_offset, {prefetch0, prefetch1}});
      ++g_items_queued;
      // tw2de (from sa2-nx) (tw2de_renut_wake_batch): a draw wakes a sleeping render thread once enough are queued.
      const bool wake = (g_render_waiting && (!draw || g_queue.size() >= WakeBatch())) ||
                        !tw2de::renut_gpu::QueueNotifyIdleOn();
      lock.unlock();
      if (wake)
        g_work_cv.notify_one();
      return;
    }
  }
  // Shutting down: nothing takes work any more, so run it here.
  Item item{std::move(work), locked, kNoTag, {}};
  Execute(item);
}

} // namespace

void Queue(std::function<void()> work) {
  Push(std::move(work), true);
}

void QueueUnlocked(std::function<void()> work) {
  Push(std::move(work), false);
}

void QueuePrefetched(const void *prefetch0, const void *prefetch1, std::function<void()> work) {
  Push(std::move(work), true, prefetch0, prefetch1, true);  // tw2de (from sa2-nx): the game's draws (tw2de_renut_wake_batch)
}

void QueueTagged(const void *tag, const void *prefetch0, const void *prefetch1, bool draw,
                 std::function<void()> work) {
  if (!prefetch0)
    prefetch0 = tag;
  const ptrdiff_t offset = static_cast<const char *>(tag) - static_cast<const char *>(prefetch0);
  const bool fits = tag && offset > INT32_MIN && offset <= INT32_MAX;
  Push(std::move(work), true, prefetch0, prefetch1, draw, fits ? int32_t(offset) : kNoTag);
}

const void *CurrentTag() {
  return t_render_thread ? g_exec_tag : nullptr;
}

bool PeekTag(uint64_t ahead, const void **tag) {
  if (!t_render_thread || !g_lockfree || g_exec_pos == UINT64_MAX || ahead == 0 || ahead >= kRingSlots / 2)
    return false;
  const uint64_t pos = g_exec_pos + ahead;
  if (!RingReady(pos))
    return false;
  *tag = ItemTag(g_ring[pos & (kRingSlots - 1)].item);
  return true;
}

void QueueFenceCompletion(u32 completion_va, u32 fence, uint64_t after_callback) {
  // TW2 (2026-11-14, tw2de_lookahead): tagged as touching no texture (kNoneTag), still without State().mutex.
  Push([completion_va, fence, after_callback] {
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      // Fences complete in order: behind one still waiting, this one waits too.
      if (g_pending_fences.empty() && after_callback <= g_callbacks_run)
        WriteFence(completion_va, fence);
      else
        g_pending_fences.push_back({completion_va, fence, after_callback});
      wake = g_done_waiters > 0 || DoneNotifyAll();  // tw2de (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  }, false, &lookahead::kNoneTag, nullptr, false, 0);
}

void QueueCallbackMarker(uint64_t seq) {
  Push([seq] {
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_callbacks_reached = std::max(g_callbacks_reached, seq);
      ++g_fences_done;  // wakes WaitForFenceProgress: there is a callback to run
      wake = g_done_waiters > 0 || DoneNotifyAll();  // tw2de (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  }, false, &lookahead::kNoneTag, nullptr, false, 0);
}

uint64_t CallbacksReached() {
  std::lock_guard lock(g_mutex);
  return g_callbacks_reached;
}

void CallbacksRun(uint64_t seq) {
  bool wake;
  {
    std::lock_guard lock(g_mutex);
    g_callbacks_run = std::max(g_callbacks_run, seq);
    size_t written = 0;
    for (; written < g_pending_fences.size(); ++written) {
      const PendingFence &p = g_pending_fences[written];
      if (p.after_callback > g_callbacks_run)
        break;
      WriteFence(p.completion_va, p.fence);
    }
    g_pending_fences.erase(g_pending_fences.begin(), g_pending_fences.begin() + written);
    wake = g_done_waiters > 0 || DoneNotifyAll();  // tw2de (from sa2-nx)
  }
  if (wake)
    g_done_cv.notify_all();
}

// tw2de (2026-10-24, the slow frames' fence waits): the render thread's side of a long fence wait, for the log.
std::string DescribeForFenceWait(FenceWaitState *state) {
  std::lock_guard lock(g_mutex);
  // TW2 (port 105): with the ring, the items queued are its claimed positions not yet taken.
  const size_t queued = LockfreeOn() ? size_t(g_enqueue.load(std::memory_order_acquire) -
                                              g_dequeue.load(std::memory_order_relaxed))
                                     : g_queue.size();
  const uint64_t queued_all = LockfreeOn() ? g_enqueue.load(std::memory_order_acquire) : g_items_queued;
  if (state)
    *state = {g_render_waiting, queued, g_pending_fences.size()};
  return fmt::format("render thread {}, {} items queued ({} queued and {} done in all), frames {} queued / {} done, "
                     "fences done {}, callbacks reached {} / run {}, {} fence completions waiting for a callback{}",
                     g_render_waiting ? "idle (waiting for work)" : "busy", queued, queued_all,
                     g_items_done, g_frames_queued, g_frames_done, g_fences_done, g_callbacks_reached,
                     g_callbacks_run, g_pending_fences.size(),
                     g_pending_fences.empty()
                         ? std::string()
                         : fmt::format(" (the first {:#x} after callback {})", g_pending_fences.front().fence,
                                       g_pending_fences.front().after_callback));
}

bool WaitForFenceProgress(std::chrono::milliseconds timeout) {
  std::unique_lock lock(g_mutex);
  if (!g_running || g_stopping)
    return false;
  const uint64_t seen = g_fences_done;
  WakeForWaitLocked();  // tw2de (from sa2-nx) (tw2de_renut_wake_batch)
  ++g_done_waiters;  // tw2de (from sa2-nx) (tw2de_renut_done_notify_waiters)
  g_done_cv.wait_for(lock, timeout,
                     [seen] { return g_fences_done != seen || !g_running || g_stopping; });
  --g_done_waiters;
  return g_running && !g_stopping;
}

// tw2de (2026-10-19, input latency, tw2de_late_input): the game reads its controller at the start of a frame, and
// when the render thread is the slower of the two its frame then waits: the Swap below waits for the frame before to be
// done. The slack of a frame is that wait plus the delay its read already had: how much later the read could have been
// without the frame reaching the render thread any later. The next frame's first read is delayed by the least slack of
// the last kSlackFrames frames less kLateInputMarginUs (rex::latency::SetFrameReadDelayFunction): a frame whose game
// work runs long eats into the margin, and a run of them shrinks the delay; with the game thread or the frame cap the
// limit there is no wait, and the delay goes to 0.
namespace {
constexpr uint32_t kSlackFrames = 16;
constexpr uint64_t kLateInputMarginUs = 2000;
// tw2de (2026-10-19, latency batch 3): the most the read is delayed: 80 % of the game's frame interval (averaged over
// the frames at QueueFrame), at most kLateInputCeilingUs. A fixed 30 ms held it back where the GPU is the limit: the
// console's 18 fps scenes (package 35) delayed 30.0 ms with more slack than that.
constexpr uint64_t kLateInputCeilingUs = 60000;
std::atomic<uint64_t> g_frame_interval_us{0};
uint64_t g_last_queue_us = 0;
uint64_t g_slack_us[kSlackFrames] = {};
uint32_t g_slack_next = 0;
uint32_t g_slack_count = 0;
std::atomic<uint32_t> g_applied_delay_us{0};
std::atomic<uint64_t> g_delay_total_us{0}, g_delay_frames{0};

// tw2de (2026-10-20, package 36s): tw2de_late_input_gpu (36l-36r), a second slack from the GPU queue's wait, is gone:
// on the console it gained nothing at the 30 fps cap and, in scenes just able to hold 30, its delay ratcheted up to
// 29 ms and cost about 6 fps (36q). docs/deko3d-renderer.md section 25.

uint32_t LateInputDelay() {
  if (!tw2de::renut_gpu::LateInputOn() || g_slack_count < kSlackFrames) {
    g_applied_delay_us.store(0, std::memory_order_relaxed);
    return 0;
  }
  uint64_t least = UINT64_MAX;
  for (uint64_t s : g_slack_us)
    least = std::min(least, s);
  const uint64_t interval = g_frame_interval_us.load(std::memory_order_relaxed);
  const uint64_t ceiling = std::min(kLateInputCeilingUs, interval * 4 / 5);
  const uint64_t delay = least > kLateInputMarginUs ? std::min(least - kLateInputMarginUs, ceiling) : 0;
  g_applied_delay_us.store(uint32_t(delay), std::memory_order_relaxed);
  g_delay_total_us.fetch_add(delay, std::memory_order_relaxed);
  g_delay_frames.fetch_add(1, std::memory_order_relaxed);
  return uint32_t(delay);
}
}  // namespace

void InstallLateInput() {
  rex::latency::SetFrameReadDelayFunction(&LateInputDelay);
}

std::string LateInputReport() {
  const uint64_t frames = g_delay_frames.exchange(0, std::memory_order_relaxed);
  const uint64_t total = g_delay_total_us.exchange(0, std::memory_order_relaxed);
  return frames ? fmt::format("late input: the first controller read of a frame delayed {:.1f} ms on average ({} "
                              "frames)",
                              double(total) / 1000.0 / double(frames), frames)
                : std::string("late input: no delay");
}

uint64_t QueueFrame(std::function<void()> work) {
  {
    std::lock_guard lock(g_mutex);
    ++g_frames_queued;
  }
  QueueUnlocked([work = std::move(work)] {
    work();
    // The frame's work includes this item's (the present); Run adds the whole
    // item afterwards, so what is left of it counts to the next frame.
    const Clock::duration so_far = Clock::now() - g_item_start;
    const double busy_ms =
        std::chrono::duration<double, std::milli>(g_busy + so_far - g_waited).count();
    g_busy = -so_far;
    g_waited = {};
    frame_stats::OnRenderThreadFrame(busy_ms);
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      ++g_frames_done;
      wake = g_done_waiters > 0 || DoneNotifyAll();  // tw2de (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
  const auto waiting = std::chrono::steady_clock::now();
  std::unique_lock lock(g_mutex);
  WakeForWaitLocked();  // tw2de (from sa2-nx) (tw2de_renut_wake_batch)
  ++g_done_waiters;  // tw2de (from sa2-nx) (tw2de_renut_done_notify_waiters)
  g_done_cv.wait(lock, [] {
    // TW2 (2026-11-12, the ultimate base's rr6_frames_ahead): how many frames the game may queue ahead (1, as before).
    static const uint64_t ahead = uint64_t(std::max(0, int(REXCVAR_GET(tw2de_frames_ahead))));
    return !g_running || g_stopping || g_frames_queued - g_frames_done <= ahead;
  });
  --g_done_waiters;
  const uint64_t done = g_frames_done;
  lock.unlock();
  const auto waited = std::chrono::steady_clock::now() - waiting;
  frame_stats::OnGameThreadWait(std::chrono::duration<double, std::milli>(waited).count());
  // tw2de (2026-10-19, tw2de_late_input): this frame's slack (the game thread only: one Swap at a time).
  {
    // The game's frame interval (the game thread only), averaged over about 16 frames.
    const uint64_t at = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (g_last_queue_us && at > g_last_queue_us) {
      const uint64_t gap = std::min<uint64_t>(at - g_last_queue_us, 1'000'000);
      const uint64_t avg = g_frame_interval_us.load(std::memory_order_relaxed);
      g_frame_interval_us.store(avg ? avg + (int64_t(gap) - int64_t(avg)) / 16 : gap, std::memory_order_relaxed);
    }
    g_last_queue_us = at;
  }
  g_slack_us[g_slack_next] =
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(waited).count()) +
      g_applied_delay_us.exchange(0, std::memory_order_relaxed);
  g_slack_next = (g_slack_next + 1) % kSlackFrames;
  g_slack_count = std::min(g_slack_count + 1, kSlackFrames);
  return done;
}

bool IsCurrent() { return t_render_thread; }
uint64_t ItemSerial() { return g_item_serial; }

uint64_t FramesDone() {
  std::lock_guard lock(g_mutex);
  return g_frames_done;
}

void Drain() {
  std::unique_lock lock(g_mutex);
  if (!g_running || std::this_thread::get_id() == g_thread_id)
    return;
  // TW2 (port 105, tw2de_renut_lockfree_queue): with the ring, the items queued are its claimed positions.
  const uint64_t target = LockfreeOn() ? g_enqueue.load(std::memory_order_acquire) : g_items_queued;
  WakeForWaitLocked();  // tw2de (from sa2-nx) (tw2de_renut_wake_batch)
  ++g_done_waiters;  // tw2de (from sa2-nx) (tw2de_renut_done_notify_waiters)
  g_done_cv.wait(lock, [target] { return !g_running || g_items_done >= target; });
  --g_done_waiters;
}

double BusyMsThisFrame() {
  return std::chrono::duration<double, std::milli>(g_busy + (Clock::now() - g_item_start) - g_waited).count();
}

void RequestIdleWork() {
  g_idle_work.store(true, std::memory_order_relaxed);
}

double WaitedMsThisFrame() {
  return std::chrono::duration<double, std::milli>(g_waited).count();
}

WaitScope::WaitScope() : start_(Clock::now()), counted_(t_render_thread) {}

WaitScope::~WaitScope() {
  if (counted_)
    g_waited += Clock::now() - start_;
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    if (!g_running || g_stopping)
      return;
    g_stopping = true;
  }
  g_accepting.store(false, std::memory_order_release);  // TW2 (port 105): later pushes run inline
  g_work_cv.notify_all();
  g_done_cv.notify_all();
}

} // namespace renut::gpu::render_thread

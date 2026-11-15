// TW2 (2026-11-12, diagnostic: the job worker crash, "call to invalid function 0x3B / 0x2"): the job batch worker
// sub_82F78458(worker r3) waits for its start semaphore (+0xC), reads the batch flag (+0x10, an atomic load), and runs
// the jobs of its list (+0x14): popped from the end of the work array (+0x10 data, +0x14 count), each called through its
// vtable +0x44 unless its "done" byte ([list +0xC] [job +0xC]) is set; the pending array (+0x20 / +0x24) is moved into
// the work array (sub_8234FE48 resize, sub_82881D50 copy) and run the same way. A crash came from a job whose first word
// was not a vtable (0x3, floats after it). With tw2de_diag_jobs each call logs (once per worker and list) which list
// it works on, and checks both arrays at entry: an entry whose vtable is not in the image is logged with the arrays.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(tw2de_diag_jobs, false, "Game/Diagnostics",
                    "Check the job batch worker's lists at each call (the job worker crash, 2026-11-12)");

REX_EXTERN(__imp__sub_82F78458);
REX_EXTERN(__imp__sub_8234FE48);

namespace {
uint32_t Be32(const uint8_t *base, uint32_t va) {
  return va ? __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + va)) : 0;
}
bool InImage(uint32_t va) { return va >= 0x82000000u && va < 0x84000000u; }

// Returns the first bad entry's index, or -1.
int CheckArray(const uint8_t *base, uint32_t data, uint32_t count, std::string &dump) {
  int bad = -1;
  if (!data || count > 500)
    return count > 500 ? 0 : -1;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t job = Be32(base, data + i * 4);
    const uint32_t vt = job ? Be32(base, job) : 0;
    if (job && !InImage(vt) && bad < 0)
      bad = int(i);
    if (i < 64)
      dump += fmt::format(" {:08X}:{:08X}", job, vt);
  }
  return bad;
}
}  // namespace

// TW2 (2026-11-12, the job system's deadlock and crash): the worker constructor (sub_82F79280) creates its thread
// running (sub_82F81BC8 with "suspended" 0; sub_828816E8 after it only sets the processor) and only then makes the
// start (+0xC) and done (+8) semaphores and sets the running flag (+0x10 = 1). A thread that reached this loop
// before that waited on handle 0 (failed at once), read the flag as 0 and returned: the worker was gone, and the
// producer's first batch waited for ever on its done semaphore (the [waits] report: 4 workers idle on their start
// semaphores, released once; the main thread on the fifth done semaphore, never released; "1 finished host
// thread" a second after the five were made). On the Xbox 360 the creating thread always won that race. Here the
// loop starts once the constructor has finished: its flag is written last.
REXCVAR_DEFINE_BOOL(tw2de_job_worker_start_wait, true, "Game/Fixes",
                    "TW2: a job worker's thread waits for its constructor (its running flag) before its loop: a "
                    "worker that started too early quit, and the game hung at its first job batch");

extern "C" REX_FUNC(sub_82F78458) {
  if (REXCVAR_GET(tw2de_job_worker_start_wait)) {
    const uint32_t worker = ctx.r3.u32;
    const auto t0 = std::chrono::steady_clock::now();
    bool waited = false;
    while (worker && Be32(base, worker + 0x10) == 0 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
      waited = true;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (waited)
      REXLOG_INFO("[jobs] worker {:08X} started before its constructor finished: waited {:.2f} ms (flag {}, start "
                  "semaphore {:08X})",
                  worker, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                  Be32(base, worker + 0x10), Be32(base, worker + 0xC));
  }
  if (REXCVAR_GET(tw2de_diag_jobs)) {
    const uint32_t worker = ctx.r3.u32, list = Be32(base, worker + 0x14);
    {
      static std::mutex m;
      static std::set<std::pair<uint32_t, uint32_t>> seen;
      std::lock_guard lock(m);
      if (seen.insert({worker, list}).second)
        REXLOG_INFO("[jobs] worker {:08X} (vtable {:08X}) list {:08X} on host thread {}", worker, Be32(base, worker),
                    list, fmt::format("{}", std::hash<std::thread::id>{}(std::this_thread::get_id())));
    }
    if (list) {
      std::string work, pending;
      const int bw = CheckArray(base, Be32(base, list + 0x10), Be32(base, list + 0x14), work);
      const int bp = CheckArray(base, Be32(base, list + 0x20), Be32(base, list + 0x24), pending);
      static std::atomic<int> logged{0};
      if ((bw >= 0 || bp >= 0) && logged.fetch_add(1) < 16)
        REXLOG_WARN("[jobs] worker {:08X} list {:08X} flag {}: bad entry work {} pending {}; work {:08X} x{}:{}; pending "
                    "{:08X} x{}:{}",
                    worker, list, Be32(base, worker + 0x10), bw, bp, Be32(base, list + 0x10), Be32(base, list + 0x14),
                    work, Be32(base, list + 0x20), Be32(base, list + 0x24), pending);
    }
  }
  const uint32_t worker_va = ctx.r3.u32;
  __imp__sub_82F78458(ctx, base);
  // At the return: the list the batch ran on (set by the producer while the worker waited).
  if (REXCVAR_GET(tw2de_diag_jobs)) {
    const uint32_t list = Be32(base, worker_va + 0x14);
    static std::mutex m;
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    std::lock_guard lock(m);
    if (seen.insert({worker_va, list}).second && seen.size() < 200)
      REXLOG_INFO("[jobs] worker {:08X} ran list {:08X} (vtable {:08X}; done bytes {:08X})", worker_va, list,
                  Be32(base, list), list ? Be32(base, list + 0xC) : 0);
  }
}

// The array resize the worker calls before it moves the pending jobs into the work array (return addresses 0x82F7857C
// and 0x82F7864C): which worker thread works on which list, once a pair.
extern "C" REX_FUNC(sub_8234FE48) {
  const uint32_t lr = uint32_t(ctx.lr);
  if ((lr == 0x82F7857Cu || lr == 0x82F7864Cu) && REXCVAR_GET(tw2de_diag_jobs)) {
    static std::mutex m;
    static std::set<std::pair<size_t, uint32_t>> seen;
    const size_t thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const uint32_t list = ctx.r3.u32 - 0x10;
    std::lock_guard lock(m);
    if (seen.insert({thread, list}).second && seen.size() < 200)
      REXLOG_INFO("[jobs] host thread {} moves pending jobs of list {:08X} ({} of them; work count {})", thread, list,
                  ctx.r4.u32, Be32(base, list + 0x14));
  }
  __imp__sub_8234FE48(ctx, base);
}

// The worker's waits for its start semaphore (WaitForSingleObject sub_82881830 from 0x82F78474 and 0x82F78714): the
// worker takes any return as "a batch is ready" (it does not look at the result), so one that returns without the
// semaphore signalled runs its list while the producer fills it. Each result other than 0 (WAIT_OBJECT_0) is logged.
REX_EXTERN(__imp__sub_82881830);
extern "C" REX_FUNC(sub_82881830) {
  const uint32_t lr = uint32_t(ctx.lr), handle = ctx.r3.u32, timeout = ctx.r4.u32;
  __imp__sub_82881830(ctx, base);
  if ((lr == 0x82F78478u || lr == 0x82F78718u) && REXCVAR_GET(tw2de_diag_jobs) && ctx.r3.u32 != 0) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 32)
      REXLOG_WARN("[jobs] the worker's wait (lr {:08X}) for handle {:08X} (timeout {:08X}) returned {:08X}", lr, handle,
                  timeout, ctx.r3.u32);
  }
}

// The job lists' copies (sub_82881D50, hooked in tw2de_native_crt.cpp, which calls this after the copy): the producer's
// share for each worker (returns 0x82F78B18, B5C, BA4, BF4, C3C), its collection of the workers' pending jobs (0x82F78CA0,
// CE8, D30, D78, DC0) and the worker's own move of its pending jobs (0x82F78594, 0x82F78660). With tw2de_diag_jobs every
// copied entry is checked (a job's first word is its vtable, in the image); the first bad ones are logged with the source.
void Tw2deCheckJobCopy(const uint8_t *base, uint32_t lr, uint32_t dst, uint32_t src, uint32_t n) {
  if (!REXCVAR_GET(tw2de_diag_jobs) || n == 0 || (n & 3) || n > 4096)
    return;
  const bool producer = lr >= 0x82F78B00u && lr <= 0x82F78E00u;
  const bool worker = lr == 0x82F78594u || lr == 0x82F78660u;
  if (!producer && !worker)
    return;
  static std::atomic<int> logged{0};
  static std::atomic<uint64_t> checked{0};
  checked.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < n / 4; ++i) {
    const uint32_t job = Be32(base, dst + i * 4);
    const uint32_t vt = job ? Be32(base, job) : 0;
    if (job && !InImage(vt)) {
      if (logged.fetch_add(1) < 24)
        REXLOG_WARN("[jobs] copy (lr {:08X}) {} entries {:08X} <- {:08X}: entry {} is {:08X} (first word {:08X}; source "
                    "entry {:08X}); {} copies checked",
                    lr, n / 4, dst, src, i, job, vt, Be32(base, src + i * 4), checked.load());
      return;
    }
  }
}

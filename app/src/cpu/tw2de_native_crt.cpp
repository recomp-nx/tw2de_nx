// tw2de (2026-10-23): the game's C runtime memory and string functions as the host C library's (tw2de_native_crt.h).
//
// Found and proven on the PC (the harness in the notes: each recompiled function against each C library function over
// 400 random inputs, r3 and a whole 1 MB arena compared; then these overrides themselves against the originals, with
// overlapping ranges and every byte value):
//   sub_82D21E80  memcpy   (926 call sites)       sub_82D21D50  memset   (620)
//   sub_82D22308  memcpy   (57)                   sub_82D223B0  memmove  (45)
//   sub_82D3DC88  memcpy   (4)                    sub_82D3DD38  memcpy   (3)
//   sub_82D3DDE8  memcpy   (6)                    sub_82D23108  strncpy  (39)
//   sub_82D21E40  strchr   (21)                   sub_82D23168  strstr   (47)
// sub_82D3DDE8 is the 360's large-copy dispatcher (XMemCpy's pattern): under 256 bytes sub_82D21E80, else by the
// destination page's protection (MmQueryAddressProtect: write-combined) sub_82D3DC88 or sub_82D3DD38, all three copies;
// on the Switch guest memory is plain RAM, the choice was only the 360's speed. strchr is the game's, not C's: it
// compares the whole 32-bit c (tw2de_native_crt.h CallStrchr). The PC check of the overrides: 3000 trials each, with
// overlaps (forward copies go to the original), exactly adjacent ranges, n = 0, pointers above 0x80000000, any c.
// Every call site was checked: none reads a volatile register (r0, r4-r12, ctr, cr0/1/5-7) after the call, which the
// native versions leave as they were (the PowerPC ABI's rule, here checked rather than assumed). Guest memory on the
// Switch is one plain block (REX_PHYS_HOST_OFFSET 0); its only unmapped pages are the GPU and XMA register windows,
// which the game reaches with no copies (0 MMIO faults in the logs), and first-touch commits retry any instruction.
// 2026-11-06: and the game's memmove sub_82729978 (its pieces as long as the overlap's distance), hooked in
// cpu/tw2de_mover_diag.cpp (CallMemmove), behind this same cvar.
// tw2de_native_crt off: the game's own functions, as before.
// tw2de (2026-11-10): FF XIII-2's (tools/match_functions.py, then the instructions compared): memcpy sub_82D21E80,
// memset sub_82D21D50, memmove sub_82D223B0, strchr sub_82D21E40, strstr sub_82D23168, the large-copy dispatcher
// sub_82D3DDE8: the same code as FF XIII's; its two page-type copies sub_82D3DC88 / sub_82D3DD38: the same
// instructions; memcpy sub_82D22308: the same less one unused neg; strncpy sub_82D23108: the same steps (the source
// byte read through an index instead of lbzu; the same zero padding). The call counts above are FF XIII's.
#include "tw2de_native_crt.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp

REXCVAR_DEFINE_BOOL(tw2de_native_crt, true, "tw2de",
                    "The game's memcpy / memset / memmove / strncpy / strchr / strstr done by the host C library "
                    "instead of the game's own recompiled ones")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
std::atomic<bool> g_logged{false};
void LogOnce() {
  if (!g_logged.load(std::memory_order_relaxed) && !g_logged.exchange(true))
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[crt] native: memcpy x3, memmove, memset, strncpy, strchr, strstr (tw2de_native_crt)");
}
}  // namespace

#define TW2DE_NATIVE_CRT(addr, call)                                     \
  REX_EXTERN(__imp__sub_##addr);                                          \
  extern "C" REX_FUNC(sub_##addr) {                                       \
    if (REXCVAR_GET(tw2de_native_crt) && tw2de::native_crt::call(ctx, base)) { \
      LogOnce();                                                          \
      return;                                                             \
    }                                                                     \
    __imp__sub_##addr(ctx, base);                                         \
  }

// TW2 (2026-11-12): the patched image's (TU1) CRT functions: LR's, found by their instructions (data and branch
// fields masked), each once in the image and the same to the end of the function: memcpy 8286C9A0 (LR 82D21E80),
// memcpy 8286CE28 (LR 82D22308), the page-type copy 82881CA0 (LR's two, 82D3DC88 / 82D3DD38, are this same code),
// memmove 82869520, memset 8286CED0, strncpy 8286D5E8, strchr 8286F490, strstr 8286F410. LR's large-copy dispatcher
// 82D3DDE8 has no counterpart: TW2's function at its place (82881D50) is another copy (VMX, prefetch) and stays the
// game's own.
TW2DE_NATIVE_CRT(8286C9A0, CallMemcpy)
TW2DE_NATIVE_CRT(8286CE28, CallMemcpy)
TW2DE_NATIVE_CRT(82881CA0, CallMemcpy)
TW2DE_NATIVE_CRT(82869520, CallMemmove)
TW2DE_NATIVE_CRT(8286CED0, CallMemset)
TW2DE_NATIVE_CRT(8286D5E8, CallStrncpy)
TW2DE_NATIVE_CRT(8286F490, CallStrchr)
TW2DE_NATIVE_CRT(8286F410, CallStrstr)

// TW2 (2026-11-12, the job worker crash): the game's VMX copy sub_82881D50 (memcpy: r3 returned; under 16 bytes
// word and byte copies through sub_8286CE28, else lvx / lvlx / stvlx / stvrx with prefetches) and VMX fill
// sub_82881A00 (memset: the byte r4, extsb, splatted) move the job lists of the job batch system (sub_82F78758 hands
// each worker its share with sub_82881D50 and clears the done bytes with sub_82881A00); a job pointer that was not one
// crashed a worker. With tw2de_native_vmx_copy they are the host's memcpy / memset (overlapping copies still go to the
// game's). tw2de_diag_vmx_copy (with the native ones off) runs the game's and checks it: the destination against a
// copy of the source taken before, and up to 32 bytes on each side of it (in its own 4 KB pages: the next page may be
// unmapped) left as they were; any difference is logged.
REXCVAR_DEFINE_BOOL(tw2de_native_vmx_copy, false, "tw2de",
                    "The game's VMX memcpy (sub_82881D50) and memset (sub_82881A00) done by the host C library")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tw2de_diag_vmx_copy, false, "Game/Diagnostics",
                    "Check the game's own VMX memcpy / memset against what they should give (with "
                    "tw2de_native_vmx_copy off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
struct VmxCheck {
  uint32_t dst = 0, n = 0, lo = 0, hi = 0;  // the bytes checked on each side: within the destination's own 4 KB pages
  std::vector<uint8_t> want, before, after;
};
void VmxPrepare(uint8_t *base, VmxCheck &c, uint32_t dst, uint32_t n, const uint8_t *src_or_null, int fill) {
  c.dst = dst;
  c.n = n;
  c.want.resize(n);
  if (src_or_null)
    std::memcpy(c.want.data(), src_or_null, n);
  else
    std::memset(c.want.data(), fill, n);
  c.lo = std::min<uint32_t>(32, dst & 0xFFFu);
  c.hi = std::min<uint32_t>(32, (0x1000u - ((dst + n) & 0xFFFu)) & 0xFFFu);
  c.before.assign(base + dst - c.lo, base + dst);
  c.after.assign(base + dst + n, base + dst + n + c.hi);
}
void VmxVerify(uint8_t *base, const VmxCheck &c, const char *what, uint32_t src, uint32_t lr) {
  const bool body = std::memcmp(base + c.dst, c.want.data(), c.n) != 0;
  // A clobber: a byte on either side, in the 16-byte block of the destination's first or last byte (where a whole
  // vector store would land), changed. Bytes further out that changed are other threads' (counted apart, not logged).
  uint32_t near = 0, far = 0;
  for (uint32_t i = 0; i < c.lo; ++i) {
    const uint32_t va = c.dst - c.lo + i;
    if (base[va] != c.before[i])
      ((va & ~15u) == (c.dst & ~15u) ? near : far)++;
  }
  for (uint32_t i = 0; i < c.hi; ++i) {
    const uint32_t va = c.dst + c.n + i;
    if (base[va] != c.after[i])
      ((va & ~15u) == ((c.dst + c.n - 1) & ~15u) ? near : far)++;
  }
  static std::atomic<int> logged{0};
  if ((body || near) && logged.fetch_add(1) < 32) {
    uint32_t first = 0;
    while (body && first < c.n && base[c.dst + first] == c.want[first])
      ++first;
    REXLOG_WARN("[crt] the game's VMX {} differs: dst {:08X} src {:08X} n {} (lr {:08X}): body {} (first byte {}), "
                "{} bytes beside it in its 16-byte blocks changed ({} further out)",
                what, c.dst, src, c.n, lr, body, first, near, far);
  }
}
}  // namespace

void Tw2deCheckJobCopy(const uint8_t *base, uint32_t lr, uint32_t dst, uint32_t src, uint32_t n);  // tw2de_job_check.cpp

REX_EXTERN(__imp__sub_82881D50);
extern "C" REX_FUNC(sub_82881D50) {
  const uint32_t d = ctx.r3.u32, s = ctx.r4.u32, n = ctx.r5.u32;
  const uint32_t copy_lr = uint32_t(ctx.lr);
  struct After {
    const uint8_t *b; uint32_t lr, d, s, n;
    ~After() { Tw2deCheckJobCopy(b, lr, d, s, n); }
  } after{base, copy_lr, d, s, n};
  if (REXCVAR_GET(tw2de_native_vmx_copy) && tw2de::native_crt::CallMemcpy(ctx, base)) {
    LogOnce();
    return;
  }
  if (REXCVAR_GET(tw2de_diag_vmx_copy) && n && n < (1u << 20) && !tw2de::native_crt::Overlap(d, n + 64, s - 32, n + 64)) {
    VmxCheck c;
    VmxPrepare(base, c, d, n, base + s, 0);
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_82881D50(ctx, base);
    VmxVerify(base, c, "memcpy", s, lr);
    return;
  }
  __imp__sub_82881D50(ctx, base);
}

REX_EXTERN(__imp__sub_82881A00);
extern "C" REX_FUNC(sub_82881A00) {
  const uint32_t d = ctx.r3.u32, v = ctx.r4.u32, n = ctx.r5.u32;
  if (REXCVAR_GET(tw2de_native_vmx_copy) && tw2de::native_crt::CallMemset(ctx, base)) {
    LogOnce();
    return;
  }
  if (REXCVAR_GET(tw2de_diag_vmx_copy) && n && n < (1u << 20)) {
    VmxCheck c;
    VmxPrepare(base, c, d, n, nullptr, int(v & 0xFF));
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_82881A00(ctx, base);
    VmxVerify(base, c, "memset", 0, lr);
    return;
  }
  __imp__sub_82881A00(ctx, base);
}

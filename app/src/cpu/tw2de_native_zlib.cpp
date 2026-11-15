// tw2de (2026-10-23): the game's zlib decompression as native zlib (devkitPro's 1.3.1, the Switch's libz.a).
//
// The game links zlib 1.1.x (its inflate: the "% 31" header check, the METHOD..BAD modes 0-13, inflate_blocks;
// adler32 at sub_82C94358 with zlib's 65521 / 5552). Its decompression goes through three functions, called from two
// places in the game (sub_8235F918, a whole buffer at once, and sub_824FCCC0, the streaming loader):
//   sub_82C93B88  inflateInit_(strm, version, stream_size)   (a jump to inflateInit2_ with windowBits 15)
//   sub_82C93A38  inflateInit2_(strm, windowBits, version, stream_size)
//   sub_82C93B98  inflate(strm, flush)
//   sub_82C939A0  inflateEnd(strm)
// Here each guest z_stream (zlib 1.1's, 56 bytes, big-endian: next_in 0, avail_in 4, total_in 8, next_out 12,
// avail_out 16, total_out 20, msg 24, state 28, zalloc 32, zfree 36, opaque 40, data_type 44, adler 48) gets a native
// z_stream; inflate runs natively straight on guest memory (the guest's address space is one block of the host's) and
// the guest's fields are brought up to date after each call. The return and flush codes are zlib's since 1.0 (Z_OK 0,
// Z_STREAM_END 1, Z_NEED_DICT 2, Z_STREAM_ERROR -2, Z_DATA_ERROR -3, Z_MEM_ERROR -4, Z_BUF_ERROR -5; Z_NO_FLUSH 0 ..
// Z_FINISH 4). The guest's state pointer is set to a non-null marker (the game checks nothing else of it).
// tw2de_native_zlib off: the game's own zlib, as before.
// tw2de (2026-11-10): FF XIII-2 links the same zlib 1.1.4 ("inflate 1.1.4 Copyright 1995-2002 Mark Adler") and
// calls the same three functions from two places (sub_8235F918, sub_824FCCC0), nothing else of it from outside; its
// inflateInit_ (sub_82C93B88: mr r6,r5; mr r5,r4; li r4,15; b inflateInit2_) is FF XIII's word for word, inflateInit2_
// (sub_82C93A38) its jump's target, inflate (sub_82C93B98) the next function, inflateEnd (sub_82C939A0) the one before.
#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-24): sdk/src/system/xobject.cpp
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>  // tw2de (2026-10-24): the store cards

#include "cpu/tw2de_wpd_patch.h"  // tw2de (2026-10-25)
#include "cpu/tw2de_texture_sources.h"  // tw2de (2026-10-25): tw2de_diag_texture_sources

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <zlib.h>

REXCVAR_DEFINE_BOOL(tw2de_native_zlib, true, "tw2de",
                    "The game's zlib decompression (its file loading) done by native zlib instead of the game's own "
                    "recompiled zlib 1.1")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82C93B88);
REX_EXTERN(__imp__sub_82C93A38);
REX_EXTERN(__imp__sub_82C93B98);
REX_EXTERN(__imp__sub_82C939A0);

namespace {

constexpr uint32_t kNext_in = 0, kAvail_in = 4, kTotal_in = 8, kNext_out = 12, kAvail_out = 16, kTotal_out = 20,
                   kMsg = 24, kState = 28, kAdler = 48;
constexpr uint32_t kStateMarker = 0x4E415449u;  // "NATI": the guest's state field while the stream is native

uint32_t Load(uint8_t *base, uint32_t ea) { return __builtin_bswap32(*reinterpret_cast<const uint32_t *>(base + ea)); }
void Store(uint8_t *base, uint32_t ea, uint32_t v) { *reinterpret_cast<uint32_t *>(base + ea) = __builtin_bswap32(v); }

struct Native {
  z_stream z{};
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Native *> g_streams;  // guest z_stream address -> native stream

std::atomic<uint64_t> g_streams_made{0}, g_bytes_in{0}, g_bytes_out{0}, g_ns{0}, g_errors{0};
std::atomic<int64_t> g_last_report{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Report() {
  const int64_t now = NowNs();
  int64_t last = g_last_report.load(std::memory_order_relaxed);
  if (now - last < 30'000'000'000ll || !g_last_report.compare_exchange_strong(last, now))
    return;
  if (!REXCVAR_GET(log_diagnostics))
    return;
  REXLOG_INFO("[zlib] native: {} streams, {:.1f} MB in, {:.1f} MB out, {:.0f} ms inflating, {} errors",
              g_streams_made.load(), double(g_bytes_in.load()) / 1048576.0, double(g_bytes_out.load()) / 1048576.0,
              double(g_ns.load()) / 1e6, g_errors.load());
}

Native *Find(uint32_t strm) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_streams.find(strm);
  return it != g_streams.end() ? it->second : nullptr;
}

// inflateInit2_(strm, windowBits, version, stream_size): the guest's own version and size checks kept (a '1' and 56).
int32_t Init(uint8_t *base, uint32_t strm, int32_t window_bits, uint32_t version, int32_t stream_size) {
  if (!version || base[version] != '1' || stream_size != 56)
    return Z_VERSION_ERROR;
  if (!strm)
    return Z_STREAM_ERROR;
  Native *n = new Native();
  if (inflateInit2(&n->z, window_bits) != Z_OK) {
    delete n;
    return Z_STREAM_ERROR;  // zlib 1.1 gave this for a bad windowBits
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Native *&slot = g_streams[strm];
    if (slot) {  // initialised again without an inflateEnd: the old one goes
      inflateEnd(&slot->z);
      delete slot;
    }
    slot = n;
  }
  Store(base, strm + kTotal_in, 0);
  Store(base, strm + kTotal_out, 0);
  tw2de::TextureSourcesForgetStream(strm);  // tw2de (2026-10-25): tw2de_diag_texture_sources
  Store(base, strm + kMsg, 0);
  Store(base, strm + kState, kStateMarker);
  Store(base, strm + kAdler, 1);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    if (REXCVAR_GET(log_diagnostics)) REXLOG_INFO("[zlib] the game's decompression is native zlib {} (tw2de_native_zlib)", zlibVersion());
  g_streams_made++;
  return Z_OK;
}

int32_t Inflate(uint8_t *base, uint32_t strm, int32_t flush) {
  Native *n = strm ? Find(strm) : nullptr;
  if (!n)
    return Z_STREAM_ERROR;
  z_stream &z = n->z;
  const uint32_t next_in = Load(base, strm + kNext_in), avail_in = Load(base, strm + kAvail_in);
  const uint32_t next_out = Load(base, strm + kNext_out), avail_out = Load(base, strm + kAvail_out);
  z.next_in = avail_in ? base + next_in : reinterpret_cast<Bytef *>(base);
  z.avail_in = avail_in;
  z.next_out = avail_out ? base + next_out : reinterpret_cast<Bytef *>(base);
  z.avail_out = avail_out;
  const int64_t t0 = NowNs();
  const int r = inflate(&z, flush);
  g_ns += uint64_t(NowNs() - t0);
  const uint32_t used_in = avail_in - z.avail_in, made_out = avail_out - z.avail_out;
  const uint32_t total_in = Load(base, strm + kTotal_in), total_out = Load(base, strm + kTotal_out);
  Store(base, strm + kNext_in, next_in + used_in);
  Store(base, strm + kAvail_in, z.avail_in);
  Store(base, strm + kTotal_in, total_in + used_in);
  Store(base, strm + kNext_out, next_out + made_out);
  Store(base, strm + kAvail_out, z.avail_out);
  Store(base, strm + kTotal_out, total_out + made_out);
  Store(base, strm + kAdler, uint32_t(z.adler));
  g_bytes_in += used_in;
  g_bytes_out += made_out;
  // tw2de (2026-10-24): the output written into guest memory marks its store cards (rex/system/xmemory.h).
  rex::memory::MarkStoreCardsAt(base, next_out, made_out);
  tw2de::WpdPatchNoteWrite(next_out, made_out);  // tw2de (2026-10-25): a language pack's changes
  if (tw2de::TextureSourcesOn())  // tw2de (2026-10-25): which file the output came from
    tw2de::TextureSourcesNoteInflate(strm, next_in, used_in, total_in, next_out, made_out, total_out);
  if (r < 0 && r != Z_BUF_ERROR) {
    if (g_errors++ < 8)
      REXLOG_WARN("[zlib] inflate of stream {:08X}: {} ({})", strm, r, z.msg ? z.msg : "");
  }
  Report();
  return r;
}

int32_t End(uint8_t *base, uint32_t strm) {
  Native *n = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = strm ? g_streams.find(strm) : g_streams.end();
    if (it == g_streams.end())
      return Z_STREAM_ERROR;
    n = it->second;
    g_streams.erase(it);
  }
  inflateEnd(&n->z);
  delete n;
  tw2de::TextureSourcesForgetStream(strm);  // tw2de (2026-10-25): tw2de_diag_texture_sources
  Store(base, strm + kState, 0);
  return Z_OK;
}

}  // namespace

// inflateInit_(strm, version, stream_size)
extern "C" REX_FUNC(sub_82C93B88) {
  if (!REXCVAR_GET(tw2de_native_zlib)) {
    __imp__sub_82C93B88(ctx, base);
    return;
  }
  ctx.r3.s64 = Init(base, ctx.r3.u32, 15, ctx.r4.u32, ctx.r5.s32);
}

// inflateInit2_(strm, windowBits, version, stream_size)
extern "C" REX_FUNC(sub_82C93A38) {
  if (!REXCVAR_GET(tw2de_native_zlib)) {
    __imp__sub_82C93A38(ctx, base);
    return;
  }
  ctx.r3.s64 = Init(base, ctx.r3.u32, ctx.r4.s32, ctx.r5.u32, ctx.r6.s32);
}

// inflate(strm, flush)
extern "C" REX_FUNC(sub_82C93B98) {
  if (!REXCVAR_GET(tw2de_native_zlib)) {
    __imp__sub_82C93B98(ctx, base);
    return;
  }
  ctx.r3.s64 = Inflate(base, ctx.r3.u32, ctx.r4.s32);
}

// inflateEnd(strm)
extern "C" REX_FUNC(sub_82C939A0) {
  if (!REXCVAR_GET(tw2de_native_zlib)) {
    __imp__sub_82C939A0(ctx, base);
    return;
  }
  ctx.r3.s64 = End(base, ctx.r3.u32);
}


// TW2 (2026-11-12): the game's USM movies (CRI Sofdec: an MPEG-1 video stream and ADX audio) decoded by FFmpeg on the
// host instead of Sofdec's MPEG decoder in guest code, which took about 110 % of a core on two threads at 1280x720.
//
// What the game does (the console profile of the intro movie, and the code):
//   sub_828BA288(dec, sj, params)  decode one picture (its header already parsed): params +32/+36/+40 the output
//                                  planes Cb, Cr, Y (128-byte aligned), +44/+46 the chroma and luma pitches; copied to
//                                  dec+4976
//   -> sub_828C2290(dec, sj)       the picture's slices: one by one (dec+5416 = 0), or handed to a thread pool
//   -> sub_828C1FA0(dec, sj, out)  one slice: its header, then the macroblock decoder at dec+5084 (I / P / B)
// The bitstream is a CRI stream joint (sj): vtable +24 GetChunk(sj, line, max, &chunk), +28 UngetChunk(sj, line,
// &chunk), +32 PutChunk(sj, line, &chunk); a chunk is {pointer, length}; line 1 holds the data, line 0 takes what was
// consumed.
//
// Here (tw2de_usm_ffmpeg):
//   sub_828C2290  runs the slices one by one (the pool off for the call), and then hands the picture to FFmpeg;
//   sub_828C1FA0  takes the slice's bytes (from its start code to the next one) off the stream joint as Sofdec would
//                 have consumed them, without decoding them (mode 1), or lets Sofdec decode them too (mode 2, verify);
//   FFmpeg (mpeg2video, which decodes MPEG-1 too; AV_CODEC_FLAG_LOW_DELAY: every picture out at once, in decode
//   order, as the guest decodes it) gets the picture header and the slices, after the sequence header when one
//   precedes the picture; its planes are written over the guest's output planes (store cards marked: Renut's
//   texture copies of them).
// Mode 2 compares FFmpeg's picture with Sofdec's, plane by plane, and logs it ([usm] lines): which plane is which and
// how far apart the two decoders' pictures are (their IDCTs round differently: a few levels at most expected).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

REXCVAR_DEFINE_INT32(tw2de_usm_ffmpeg, 1, "tw2de",  // TW2 (2026-11-12): 1 by default after the intro movie's compare (mode 2: |Sofdec - FFmpeg| 0.01-0.03 a pixel) and run (CPU 216-243 % -> 49-71 %)
                     "TW2: the USM movies' MPEG video decoded by FFmpeg on the host: 0 Sofdec's decoder (guest code), "
                     "1 FFmpeg, 2 both, compared ([usm] log lines; Sofdec's picture is shown)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(tw2de_usm_planes, "UVY", "tw2de",
                      "TW2: which picture plane each of the guest's three output planes (params +32, +36, +40) is "
                      "(from tw2de_usm_ffmpeg 2's [usm] lines: +32 Cb, +36 Cr, +40 Y in the intro movie)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(tw2de_usm_black_incomplete, true, "tw2de",
                    "TW2: a movie picture that lost slices (a skipped movie's stream cut mid-picture) or that FFmpeg "
                    "marks as damaged is shown black instead of half decoded (with tw2de_usm_ffmpeg 1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_828C2290);
REX_EXTERN(__imp__sub_828C1FA0);
REX_EXTERN(__imp__sub_828BB068);

namespace {

constexpr uint32_t kDecPlanes = 4976 + 32;  // dec+4976: the 64 bytes of params
constexpr uint32_t kDecPitches = 4976 + 44;
constexpr uint32_t kDecChunk = 5288;
constexpr uint32_t kDecPool = 5416;
constexpr uint32_t kDecLastMb = 5204;
constexpr uint32_t kDecMbCount = 5216;
constexpr uint32_t kDecSliceErrors = 5408;

uint32_t Ld32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}
uint16_t Ld16(const uint8_t *base, uint32_t va) {
  uint16_t v;
  std::memcpy(&v, base + va, 2);
  return __builtin_bswap16(v);
}
void St32(uint8_t *base, uint32_t va, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + va, &v, 4);
  rex::memory::MarkStoreCardsAt(base, va, 4);
}

struct Movie {
  std::mutex mutex;
  AVCodecContext *avctx = nullptr;
  AVPacket *packet = nullptr;
  AVFrame *frame = nullptr;
  bool have_sequence = false;
  std::vector<uint8_t> bytes;  // this picture's: [sequence header,] picture header, slices
  uint32_t slices = 0;
  bool header_found = false;
  bool unterminated = false;
  uint64_t pictures = 0, decoded = 0, failed = 0, compared = 0, blacked = 0;
  uint32_t width = 0, height = 0;  // the last picture's (BlackPlanes)
  // TW2 (2026-11-13, the user's report: a grey or purple frame when the first prologue movie starts or is skipped):
  // the reference pictures FFmpeg has of this stream (0-2), and when the last picture came. A new stream (a sequence
  // header after a pause) drops the old movie's references; a P picture without one, or a B picture without two (an
  // open GOP's leading B pictures at a movie's start), is drawn by FFmpeg against a grey stand-in or a picture of the
  // movie before: those are written black.
  uint32_t refs = 0;
  std::chrono::steady_clock::time_point last_picture{};
  uint64_t missing_refs = 0;
  uint64_t bad_sequences = 0;
};

std::mutex g_movies_mutex;
std::unordered_map<uint32_t, Movie *> g_movies;  // by the guest decoder's address; never freed (a few a game)

Movie &MovieFor(uint32_t dec) {
  std::lock_guard lock(g_movies_mutex);
  Movie *&m = g_movies[dec];
  if (!m)
    m = new Movie();
  return *m;
}

struct Current {
  uint32_t dec = 0;
  Movie *movie = nullptr;
  int mode = 0;
};
thread_local Current t_cur;

// The next start code (00 00 01 xx) in [p + from, p + n): its offset, or n.
uint32_t NextStartCode(const uint8_t *p, uint32_t from, uint32_t n) {
  for (uint32_t i = from; i + 3 < n; ++i)
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
      return i;
  return n;
}

// A guest function through a pointer (the stream joint's vtable).
void CallGuest(PPCContext &ctx, uint8_t *base, uint32_t address) {
  PPCFunc *f = rex::runtime::ResolveIndirectFunction(address);
  f(ctx, base);
}

// Before the picture's first slice: its picture header, and the sequence header (and GOP header) before that when
// there is one, from the memory just before the slice (consumed a moment ago, still there).
void TakeHeaders(Movie &m, uint8_t *base, uint32_t slice_va) {
  constexpr uint32_t kBack = 4096;
  const uint32_t from = slice_va > kBack ? slice_va - kBack : 0;
  const uint8_t *p = base + from;
  const uint32_t n = slice_va - from;
  int64_t picture = -1, sequence = -1;
  for (int64_t i = int64_t(n) - 4; i >= 0; --i) {
    if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
      continue;
    if (picture < 0) {
      if (p[i + 3] == 0x00)
        picture = i;
      else if (p[i + 3] != 0xB5 && p[i + 3] != 0xB2)  // extensions and user data may follow the picture header
        break;
      continue;
    }
    if (p[i + 3] == 0xB3) {
      sequence = i;
      break;
    }
    if (p[i + 3] != 0xB8 && p[i + 3] != 0xB5 && p[i + 3] != 0xB2)  // GOP header, extensions, user data
      break;
  }
  if (picture < 0)
    return;
  // TW2 (2026-11-13): a sequence header found this way must be a sane one: the bytes before the slice are whatever the
  // stream buffer last held there, and a stale 00 00 01 B3 ("frame_rate_index 15 is invalid" in the log) made FFmpeg
  // drop its reference pictures (the grey frame of the user's report). Width and height not 0, aspect ratio code 1-4,
  // frame rate code 1-8; else only the picture header is taken.
  if (sequence >= 0) {
    const uint8_t *q = p + sequence + 4;
    const bool sane = sequence + 8 <= picture && ((q[0] << 4) | (q[1] >> 4)) != 0 &&
                      (((q[1] & 0xF) << 8) | q[2]) != 0 && (q[3] >> 4) >= 1 && (q[3] >> 4) <= 4 &&
                      (q[3] & 0xF) >= 1 && (q[3] & 0xF) <= 8;
    if (!sane) {
      if (m.bad_sequences++ < 8)
        REXLOG_INFO("[usm] a sequence header before a picture is not a sane one ({:02X} {:02X} {:02X} {:02X}): left out",
                    q[0], q[1], q[2], q[3]);
      sequence = -1;
    }
  }
  m.header_found = true;
  const int64_t start = sequence >= 0 ? sequence : picture;
  if (sequence >= 0)
    m.have_sequence = true;
  m.bytes.insert(m.bytes.end(), p + start, p + n);
}

// TW2 (2026-11-13): FFmpeg's messages. A picture decoded against a stand-in reference (mpegvideo.c ff_mpv_frame_start:
// "first frame is no keyframe" / "allocating dummy last picture": grey 0x80, the grey frame of the user's report)
// is marked for this thread's picture; FFmpeg's warnings and errors go to the log (the first 32).
thread_local bool t_stand_in_ref = false;
void AvLogCallback(void *, int level, const char *fmt, va_list vl) {
  if (!fmt)
    return;
  if (std::strstr(fmt, "no keyframe") || std::strstr(fmt, "dummy"))
    t_stand_in_ref = true;
  if (level > AV_LOG_WARNING)
    return;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) >= 32)
    return;
  char line[256];
  std::vsnprintf(line, sizeof(line), fmt, vl);
  size_t n = std::strlen(line);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
    line[--n] = 0;
  REXLOG_INFO("[usm] FFmpeg: {}", line);
}

bool OpenDecoder(Movie &m) {
  if (m.avctx)
    return true;
  static std::once_flag log_once;
  std::call_once(log_once, [] { av_log_set_callback(AvLogCallback); });
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
  if (!codec)
    return false;
  m.avctx = avcodec_alloc_context3(codec);
  m.avctx->flags |= AV_CODEC_FLAG_LOW_DELAY;  // each picture out as it is decoded (the guest's order)
  m.avctx->thread_count = 1;
  if (avcodec_open2(m.avctx, codec, nullptr) < 0) {
    avcodec_free_context(&m.avctx);
    return false;
  }
  m.packet = av_packet_alloc();
  m.frame = av_frame_alloc();
  return true;
}

int PlaneIndex(char c) { return c == 'Y' ? 0 : c == 'U' ? 1 : c == 'V' ? 2 : -1; }

// TW2 (2026-11-12): the guest's output planes black (luma 0, chroma 128) at the movie's last size: a picture FFmpeg
// could not decode (mode 1 only; Sofdec decoded nothing either) is black, not whatever the buffer held before.
void BlackPlanes(Movie &m, uint8_t *base, uint32_t dec) {
  if (!m.width || !m.height || !REXCVAR_GET(tw2de_usm_black_incomplete))
    return;
  const uint32_t pitch_c = Ld16(base, dec + kDecPitches), pitch_y = Ld16(base, dec + kDecPitches + 2);
  for (int g = 0; g < 3; ++g) {
    const uint32_t plane = Ld32(base, dec + kDecPlanes + uint32_t(g) * 4);
    const bool luma = g == 2;  // +32 Cb, +36 Cr, +40 Y
    const uint32_t pw = luma ? m.width : m.width / 2, ph = luma ? m.height : m.height / 2;
    const uint32_t pitch = luma ? pitch_y : pitch_c;
    if (!plane || pitch < pw)
      continue;
    for (uint32_t y = 0; y < ph; ++y)
      std::memset(base + plane + y * pitch, luma ? 0 : 128, pw);
    rex::memory::MarkStoreCardsAt(base, plane, pitch * ph);
  }
}

// FFmpeg's picture into the guest's planes (mode 1), or compared with them (mode 2).
void FinishPicture(Movie &m, uint8_t *base, uint32_t dec, int mode) {
  ++m.pictures;
  if (m.bytes.empty() || !m.slices || !m.header_found || !m.have_sequence || !OpenDecoder(m)) {
    ++m.failed;
    if (m.failed <= 8)
      REXLOG_WARN("[usm] {:08X}: picture {} not decoded by FFmpeg ({} slices, header {}, sequence {})", dec,
                  m.pictures, m.slices, m.header_found, m.have_sequence);
    if (mode == 1)
      BlackPlanes(m, base, dec);
    return;
  }
  {
    const auto now = std::chrono::steady_clock::now();
    const bool sequence_first = m.bytes.size() > 4 && m.bytes[0] == 0 && m.bytes[1] == 0 && m.bytes[2] == 1 &&
                                m.bytes[3] == 0xB3;
    if (sequence_first && m.last_picture.time_since_epoch().count() != 0 &&
        now - m.last_picture > std::chrono::milliseconds(250)) {
      avcodec_flush_buffers(m.avctx);
      m.refs = 0;
    }
    m.last_picture = now;
  }
  av_packet_unref(m.packet);
  if (av_new_packet(m.packet, int(m.bytes.size())) < 0)
    return;
  std::memcpy(m.packet->data, m.bytes.data(), m.bytes.size());
  int got = 0;
  t_stand_in_ref = false;
  if (avcodec_send_packet(m.avctx, m.packet) >= 0) {
    while (avcodec_receive_frame(m.avctx, m.frame) >= 0) {
      got = 1;
      break;  // one picture a packet (low delay)
    }
  }
  if (!got) {
    ++m.failed;
    if (m.failed <= 8)
      REXLOG_WARN("[usm] {:08X}: FFmpeg gave no picture for picture {} ({} bytes, {} slices)", dec, m.pictures,
                  m.bytes.size(), m.slices);
    if (mode == 1)
      BlackPlanes(m, base, dec);
    return;
  }
  ++m.decoded;
  const AVFrame *f = m.frame;
  const uint32_t planes[3] = {Ld32(base, dec + kDecPlanes), Ld32(base, dec + kDecPlanes + 4),
                              Ld32(base, dec + kDecPlanes + 8)};
  // +44 the chroma planes' pitch, +46 the luma plane's (mode 2's first run: 640 / 1280 at 1280x720)
  const uint32_t pitch_c = Ld16(base, dec + kDecPitches), pitch_y = Ld16(base, dec + kDecPitches + 2);
  const int w = f->width, h = f->height;
  m.width = uint32_t(w);
  m.height = uint32_t(h);
  if (mode == 2) {
    if (m.compared++ < 24) {
      // Every guest plane against every FFmpeg plane: the mean absolute difference over the top rows.
      std::string line;
      for (int g = 0; g < 3; ++g) {
        for (int k = 0; k < 3; ++k) {
          const int pw = k ? w / 2 : w, ph = k ? h / 2 : h;
          const uint32_t pitch = k ? pitch_c : pitch_y;
          const int rows = ph < 64 ? ph : 64;
          uint64_t sum = 0;
          for (int y = 0; y < rows; ++y) {
            const uint8_t *a = base + planes[g] + uint32_t(y) * pitch;
            const uint8_t *b = f->data[k] + y * f->linesize[k];
            for (int x = 0; x < pw; ++x)
              sum += uint32_t(a[x] > b[x] ? a[x] - b[x] : b[x] - a[x]);
          }
          line += fmt::format(" g{}-{}c {:.2f}", g, "YUV"[k], double(sum) / double(rows * pw));
        }
      }
      REXLOG_INFO("[usm] {:08X} picture {} type {} {}x{} pitches {}/{} planes {:08X} {:08X} {:08X}; {} bytes {} "
                  "slices{}; mean |Sofdec - FFmpeg| by plane:{}",
                  dec, m.pictures, int(f->pict_type), w, h, pitch_y, pitch_c, planes[0], planes[1], planes[2],
                  m.bytes.size(), m.slices, m.unterminated ? " (a slice ran to its chunk's end)" : "", line);
    }
    return;
  }
  // TW2 (2026-11-12, the user's report: a skipped movie's last frame shows up broken): a picture that lost slices (the
  // stream cut off mid-picture when a movie is skipped: fewer slices than macroblock rows) or that FFmpeg flags as
  // damaged is written black (tw2de_usm_black_incomplete) instead of a half-decoded picture.
  const uint32_t rows = uint32_t(h + 15) / 16;
  const bool damaged = f->decode_error_flags != 0 || (f->flags & AV_FRAME_FLAG_CORRUPT) != 0;
  bool missing_ref = t_stand_in_ref;
  if (f->pict_type == AV_PICTURE_TYPE_B) {
    missing_ref = missing_ref || m.refs < 2;
  } else {
    missing_ref = missing_ref || (f->pict_type != AV_PICTURE_TYPE_I && m.refs < 1);
    m.refs = m.refs < 2 ? m.refs + 1 : 2;
  }
  const bool incomplete = REXCVAR_GET(tw2de_usm_black_incomplete) && (m.slices < rows || damaged || missing_ref);
  if (incomplete && m.blacked++ < 16)
    REXLOG_INFO("[usm] {:08X}: picture {} (type {}) incomplete ({} of {} slices{}{}): shown black", dec, m.pictures,
                av_get_picture_type_char(f->pict_type), m.slices, rows, damaged ? ", FFmpeg: damaged" : "",
                missing_ref ? ", its reference pictures missing" : "");
  static const std::string order = [] {
    std::string s = REXCVAR_GET(tw2de_usm_planes);
    return s.size() == 3 ? s : std::string("UVY");
  }();
  for (int g = 0; g < 3; ++g) {
    const int k = PlaneIndex(order[g]);
    if (k < 0 || !planes[g])
      continue;
    const int pw = k ? w / 2 : w, ph = k ? h / 2 : h;
    const uint32_t pitch = k ? pitch_c : pitch_y;
    if (pitch < uint32_t(pw))
      continue;
    for (int y = 0; y < ph; ++y) {
      if (incomplete)  // black: luma 0, chroma 128
        std::memset(base + planes[g] + uint32_t(y) * pitch, k ? 128 : 0, size_t(pw));
      else
        std::memcpy(base + planes[g] + uint32_t(y) * pitch, f->data[k] + y * f->linesize[k], size_t(pw));
    }
    rex::memory::MarkStoreCardsAt(base, planes[g], pitch * uint32_t(ph));
  }
}

}  // namespace

// The picture's slices.
extern "C" REX_FUNC(sub_828C2290) {
  const int mode = REXCVAR_GET(tw2de_usm_ffmpeg);
  if (mode == 0) {
    __imp__sub_828C2290(ctx, base);
    return;
  }
  const uint32_t dec = ctx.r3.u32;
  Movie &m = MovieFor(dec);
  std::lock_guard lock(m.mutex);
  m.bytes.clear();
  m.slices = 0;
  m.header_found = false;
  m.unterminated = false;
  t_cur = {dec, &m, mode};
  const uint32_t pool = Ld32(base, dec + kDecPool);
  St32(base, dec + kDecPool, 0);  // one by one, on this thread, through sub_828C1FA0 below
  __imp__sub_828C2290(ctx, base);
  St32(base, dec + kDecPool, pool);
  t_cur = {};
  const uint64_t result = ctx.r3.u64;
  FinishPicture(m, base, dec, mode);
  ctx.r3.u64 = result;
}

// One slice.
extern "C" REX_FUNC(sub_828C1FA0) {
  const Current cur = t_cur;
  const uint32_t dec = ctx.r3.u32, sj = ctx.r4.u32, out = ctx.r5.u32;
  if (!cur.movie || cur.dec != dec) {
    __imp__sub_828C1FA0(ctx, base);
    return;
  }
  Movie &m = *cur.movie;
  const uint32_t vtable = Ld32(base, sj);
  // The slice's bytes: GetChunk(sj, 1, max, &dec->chunk).
  ctx.r3.u64 = sj;
  ctx.r4.u64 = 1;
  ctx.r5.u64 = 0x7FFFFFFF;
  ctx.r6.u64 = dec + kDecChunk;
  CallGuest(ctx, base, Ld32(base, vtable + 24));
  const uint32_t ptr = Ld32(base, dec + kDecChunk), len = Ld32(base, dec + kDecChunk + 4);
  uint32_t end = len, next = 0;
  bool last = true;
  if (ptr && len >= 4) {
    end = NextStartCode(base + ptr, 4, len);
    if (end + 3 < len) {
      next = base[ptr + end + 3];
      last = !(next >= 0x01 && next <= 0xAF);
    } else {
      m.unterminated = true;
    }
    if (!m.slices)
      TakeHeaders(m, base, ptr);
    m.bytes.insert(m.bytes.end(), base + ptr, base + ptr + end);
    ++m.slices;
  }
  if (cur.mode == 2) {
    // Sofdec decodes it too: the chunk back as it was, then the original.
    const uint32_t r1 = ctx.r1.u32;
    ctx.r3.u64 = sj;
    ctx.r4.u64 = 1;
    ctx.r5.u64 = dec + kDecChunk;
    CallGuest(ctx, base, Ld32(base, vtable + 28));
    ctx.r1.u64 = r1;
    ctx.r3.u64 = dec;
    ctx.r4.u64 = sj;
    ctx.r5.u64 = out;
    __imp__sub_828C1FA0(ctx, base);
    return;
  }
  // Consumed: {ptr, end} into line 0, the rest {ptr + end, len - end} back into line 1 (sub_828C3A98's split).
  const uint32_t r1 = ctx.r1.u32;
  ctx.r1.u64 = r1 - 128;  // a guest stack frame for the rest's chunk
  const uint32_t rest = ctx.r1.u32 + 80;
  St32(base, dec + kDecChunk, ptr);
  St32(base, dec + kDecChunk + 4, end);
  St32(base, rest, end < len ? ptr + end : 0);
  St32(base, rest + 4, len - end);
  ctx.r3.u64 = sj;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = dec + kDecChunk;
  CallGuest(ctx, base, Ld32(base, vtable + 32));
  ctx.r3.u64 = sj;
  ctx.r4.u64 = 1;
  ctx.r5.u64 = rest;
  CallGuest(ctx, base, Ld32(base, vtable + 28));
  ctx.r3.u64 = sj;
  __imp__sub_828BB068(ctx, base);  // as the macroblock decoder ends: the next start code looked at
  ctx.r1.u64 = r1;
  // The slice's results as Sofdec leaves them: no errors, the picture done after its last slice.
  St32(base, dec + kDecSliceErrors, 0);
  const uint32_t mbs = Ld32(base, dec + kDecMbCount);
  St32(base, dec + kDecLastMb, last ? mbs : mbs - 1);
  St32(base, out + 0, 0);
  St32(base, out + 4, 0);
  St32(base, out + 8, 0);
  St32(base, out + 12, 0xFFFFFFFFu);
  St32(base, out + 16, 0xFFFFFFFFu);
  St32(base, out + 20, last ? 1u : 0u);
  ctx.r3.u64 = 0;
}

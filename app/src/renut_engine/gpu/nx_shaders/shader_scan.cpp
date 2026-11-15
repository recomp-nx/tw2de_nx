// tw2de (2026-10-21): shader_scan.h. The rules are tools/tw2de_scan_shaders.py's (scan_headers, ucode_units,
// valid_ucode, scan_chunk's zlib walk); keep them in step.
#include "renut_engine/gpu/nx_shaders/shader_scan.h"
#include "renut_engine/gpu/nx_shaders/nx_file.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <mutex>
#include <vector>

#include <zlib.h>
#include <switch.h>  // svcSleepThread

namespace renut::nxs {

namespace {

constexpr uint32_t kMaxCfUnits = 128;
constexpr size_t kMaxStreamOut = 128u << 20;  // the script's 256 MB; the game's largest stream with shaders is far less
constexpr size_t kRawMargin = 0x110000 + 0x1000;  // a raw header's whole container: virtual 64 KB + physical 1 MB

inline uint32_t Be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

inline bool IsExec(uint32_t op) { return (op >= 1 && op <= 6) || op == 13 || op == 14; }
inline bool IsEnd(uint32_t op) { return op == 2 || op == 4 || op == 6 || op == 14; }

// ucode_units: the length in instructions of well-formed microcode at buf[x], or 0.
size_t UcodeUnits(const uint8_t *buf, size_t n, size_t x) {
  uint32_t bound = 4096;
  struct Cf {
    uint32_t op, lo;
  };
  Cf cfs[2 * (kMaxCfUnits + 1)];
  size_t count = 0;
  for (uint32_t i = 0; i < bound; i++) {
    const size_t o = x + 12 * size_t(i);
    if (o + 12 > n || i > kMaxCfUnits)
      return 0;
    const uint32_t d0 = Be32(buf + o), d1 = Be32(buf + o + 4), d2 = Be32(buf + o + 8);
    const uint32_t pairs[2][2] = {{d0, d1 & 0xFFFF}, {(d1 >> 16) | (d2 << 16), d2 >> 16}};
    for (const auto &pr : pairs) {
      const uint32_t op = (pr[1] >> 12) & 0xF;
      cfs[count++] = {op, pr[0]};
      if (IsExec(op)) {
        const uint32_t addr = pr[0] & 0xFFF;
        if (addr == 0)
          return 0;
        bound = std::min(bound, addr);
      }
    }
  }
  bool ended = false, any = false;
  uint32_t lowest = UINT32_MAX, top = 0;
  uint64_t covered[65] = {};  // instruction indices < 4096 + 7
  size_t covered_count = 0;
  for (size_t k = 0; k < count; k++) {
    const Cf &c = cfs[k];
    if (IsExec(c.op)) {
      const uint32_t addr = c.lo & 0xFFF, cnt = (c.lo >> 12) & 7;
      if (addr < bound)
        return 0;
      for (uint32_t j = addr; j < addr + cnt; j++) {
        if (!(covered[j >> 6] & (1ull << (j & 63)))) {
          covered[j >> 6] |= 1ull << (j & 63);
          covered_count++;
        }
        lowest = std::min(lowest, j);
        top = std::max(top, j + 1);
      }
      any = any || cnt != 0;
      ended = ended || IsEnd(c.op);
    }
    else if ((c.op == 9 || c.op == 11) && (c.lo & 0x1FFF) >= count) {
      return 0;
    }
  }
  if (!ended || !any)
    return 0;
  if (covered_count != top - bound || lowest != bound)
    return 0;
  const size_t units = size_t(top) + 1;  // the compiler leaves one instruction slot after the last clause
  if (x + 12 * units > n)
    return 0;
  return units;
}

bool ValidUcode(const uint8_t *p, size_t size) {
  if (size < 24 || size % 12)
    return false;
  const size_t u = UcodeUnits(p, size, 0);
  return u * 12 == size || (u != 0 && (u - 1) * 12 == size);
}

// scan_headers: the containers whose headers start in buf[lo, hi).
void ScanHeaders(const uint8_t *buf, size_t len, size_t lo, size_t hi, const ContainerSink &sink, ScanStats &stats) {
  hi = std::min(hi, len);
  size_t o = lo;
  while (o + 3 <= hi) {
    const void *m = std::memchr(buf + o, 0x10, hi - o);
    if (!m)
      break;
    o = size_t(static_cast<const uint8_t *>(m) - buf);
    if (o + 3 > len || buf[o + 1] != 0x2A || buf[o + 2] != 0x11 || o >= hi) {
      o++;
      continue;
    }
    const size_t at = o;
    o += 3;
    if (at + 0x24 > len)
      continue;
    const uint8_t *h = buf + at;
    const uint32_t vsize = Be32(h + 4), psize = Be32(h + 8), ctab = Be32(h + 16), dtab = Be32(h + 20),
                   shoff = Be32(h + 24);
    if (!(vsize >= 0x24 && vsize <= 0x10000 && psize > 0 && psize <= 0x100000 && psize % 4 == 0))
      continue;
    if ((ctab && ctab >= vsize) || (dtab && dtab >= vsize) || shoff < 0x24 || shoff + 0x18 > vsize)
      continue;
    if (at + vsize > len)
      continue;
    const uint32_t po = Be32(h + shoff), usize = Be32(h + shoff + 4);
    if (usize == 0 || usize % 12 || uint64_t(po) + usize > psize)
      continue;
    stats.headers.fetch_add(1, std::memory_order_relaxed);
    const size_t end = at + vsize + psize;
    if (end <= len && ValidUcode(buf + at + vsize + po, psize - po)) {
      stats.containers.fetch_add(1, std::memory_order_relaxed);
      sink(buf + at, vsize + psize);
    }
  }
}


// The game file through libnx (nx_file.h: FsFile), read-only.
struct File {
  NxFile f;
  uint64_t size = 0;
  const std::string *lazy_path = nullptr;  // tw2de (2026-11-10): opened at its first read (a piece's stream past it)
  bool Read(uint64_t at, uint8_t *dst, size_t n) {
    std::lock_guard<NxMutex> lock(CardMutex());  // one card operation at a time (nx_file.h)
    if (!f.IsOpen() && (!lazy_path || !f.OpenRead(*lazy_path)))
      return false;
    return f.Read(int64_t(at), dst, n);
  }
};

}  // namespace

namespace {

// The headers and streams starting in data[0, range): data holds len bytes of the file from 'begin'; a stream that
// runs past them goes on from the file (none when data is the whole file).
bool ScanBuffer(File &file, uint64_t begin, const uint8_t *data, size_t len, size_t range, const ContainerSink &sink,
                ScanStats &stats, const std::atomic<bool> *cancel) {
  // Raw bytes: headers stored uncompressed.
  ScanHeaders(data, len, 0, range, sink, stats);

  // zlib streams starting in the range (78 01 / 5E / 9C / DA).
  // tw2de (2026-10-21): the inflate output and the file's input, kept per thread at the largest size reached (no
  // regrowth, no zero fill per stream: the stream writes what is read).
  struct Grow {
    std::unique_ptr<uint8_t[]> p;
    size_t cap = 0;
    uint8_t *data() { return p.get(); }
    // At least n bytes, the first 'keep' of them kept.
    bool Ensure(size_t n, size_t keep = 0) {
      if (n <= cap)
        return true;
      std::unique_ptr<uint8_t[]> q(new (std::nothrow) uint8_t[n]);
      if (!q)
        return false;
      if (keep)
        std::memcpy(q.get(), p.get(), keep);
      p = std::move(q);
      cap = n;
      return true;
    }
  };
  thread_local Grow out_buf, more;
  z_stream z{};
  if (inflateInit(&z) != Z_OK)
    return false;
  size_t skip_to = 0;
  size_t reported = 0;
  for (size_t o = 0; o + 1 < range; o++) {
    if (o - reported >= (1u << 20)) {
      stats.bytes.fetch_add(o - reported, std::memory_order_relaxed);
      reported = o;
      if (cancel && cancel->load(std::memory_order_relaxed)) {
        inflateEnd(&z);
        return false;
      }
    }
    const void *m = std::memchr(data + o, 0x78, range - 1 - o);
    if (!m)
      break;
    o = size_t(static_cast<const uint8_t *>(m) - data);
    if (o + 1 >= range)
      break;
    const uint8_t b1 = data[o + 1];
    if (o < skip_to || (b1 != 0x01 && b1 != 0x5E && b1 != 0x9C && b1 != 0xDA))
      continue;
    // Inflate: the buffer's bytes first, then the file's (a stream past the buffer).
    inflateReset(&z);
    if (!out_buf.Ensure(64u << 10))
      break;
    size_t limit = std::min(out_buf.cap, kMaxStreamOut);  // what this stream may fill before the buffer grows
    size_t in_at = o;                    // into the buffer
    uint64_t file_at = begin + len;      // when the buffer is used up
    bool from_file = false, failed = false, eof = false;
    // The first 512 bytes on their own, as the script (most false starts fail there).
    z.next_in = const_cast<uint8_t *>(data + in_at);
    z.avail_in = unsigned(std::min<size_t>(512, len - in_at));
    in_at += z.avail_in;
    size_t produced = 0;
    for (;;) {
      if (produced == limit) {
        if (limit >= kMaxStreamOut)
          break;
        if (!out_buf.Ensure(std::min(kMaxStreamOut, limit * 2), produced))
          break;
        limit = std::min(out_buf.cap, kMaxStreamOut);
      }
      z.next_out = out_buf.data() + produced;
      z.avail_out = unsigned(limit - produced);
      const int r = inflate(&z, Z_NO_FLUSH);
      produced = limit - z.avail_out;
      if (r == Z_STREAM_END) {
        eof = true;
        break;
      }
      if (r == Z_NEED_DICT || r == Z_DATA_ERROR || r == Z_MEM_ERROR || r == Z_STREAM_ERROR) {
        failed = true;
        break;
      }
      if (z.avail_in == 0) {
        // More input: the rest of the buffer, then the file in 4 MB pieces.
        if (!from_file && in_at < len) {
          const size_t n = std::min<size_t>(4u << 20, len - in_at);
          z.next_in = const_cast<uint8_t *>(data + in_at);
          z.avail_in = unsigned(n);
          in_at += n;
        }
        else if (file_at < file.size) {
          from_file = true;
          const size_t n = size_t(std::min<uint64_t>(4u << 20, file.size - file_at));
          if (!more.Ensure(n)) {
            failed = true;
            break;
          }
          if (!file.Read(file_at, more.data(), n)) {
            failed = true;
            break;
          }
          file_at += n;
          z.next_in = more.data();
          z.avail_in = unsigned(n);
        }
        else {
          break;  // the file ends inside the stream
        }
      }
      else if (r == Z_BUF_ERROR && z.avail_out != 0) {
        failed = true;
        break;
      }
    }
    if (failed || produced < 0x40)
      continue;
    if (eof)
      skip_to = o + size_t(z.total_in);  // the next stream starts after this one
    stats.streams.fetch_add(1, std::memory_order_relaxed);
    ScanHeaders(out_buf.data(), produced, 0, produced, sink, stats);
  }
  inflateEnd(&z);
  // A rare huge stream's buffer is not kept for the rest of the scan.
  if (out_buf.cap > (16u << 20)) {
    out_buf.p.reset();
    out_buf.cap = 0;
  }
  stats.bytes.fetch_add(range - reported, std::memory_order_relaxed);
  return true;
}

}  // namespace

bool ScanFileRange(const std::string &path, uint64_t begin, uint64_t end, const ContainerSink &sink, ScanStats &stats,
                   const std::atomic<bool> *cancel) {
  File file;
  if (!file.f.OpenRead(path))
    return false;
  const int64_t file_size = file.f.Size();
  if (file_size < 0)
    return false;
  file.size = uint64_t(file_size);
  end = std::min(end, file.size);
  if (begin >= end)
    return true;
  // The range, and after it what a header near its end or the start of a stream may need.
  const uint64_t buf_end = std::min(file.size, end + std::max<uint64_t>(kRawMargin, 4u << 20));
  // tw2de (2026-11-10): one buffer per thread, allocated at its first range and kept (the ranges are all
  // kScanChunk + margin at most). FF XIII-2's runs fragmented newlib's heap: whole files of 150-490 MB came and went
  // between the small blocks that stay (the stores' entries), the heap grew to the whole process (3.1 GB) and a
  // 36 MB block was then not to be had (std::bad_alloc, then ranges given up as "could not be read").
  // Still without throwing: waited for (up to 30 s), then the range is given up and the file is scanned again at the
  // next start.
  const size_t n = size_t(buf_end - begin);
  thread_local std::unique_ptr<uint8_t[]> t_buf;
  thread_local size_t t_cap = 0;
  if (n > t_cap) {
    const size_t want = std::max<size_t>(n, size_t(kScanChunk) + std::max<size_t>(kRawMargin, 4u << 20));
    t_buf.reset();
    t_cap = 0;
    for (int tries = 0; tries < 150 && !t_buf; tries++) {
      t_buf.reset(new (std::nothrow) uint8_t[want]);
      if (!t_buf) {
        if (cancel && cancel->load(std::memory_order_relaxed))
          return false;
        svcSleepThread(200'000'000);
      }
    }
    if (!t_buf)
      return false;
    t_cap = want;
  }
  uint8_t *const buf = t_buf.get();
  if (!file.Read(begin, buf, n))
    return false;
  return ScanBuffer(file, begin, buf, n, size_t(end - begin), sink, stats, cancel);
}

ScanResult LoadWholeFile(const std::string &path, std::shared_ptr<const uint8_t> &data, uint64_t &size,
                         const std::atomic<bool> *cancel) {
  File file;
  if (!file.f.OpenRead(path))
    return ScanResult::kUnreadable;
  const int64_t file_size = file.f.Size();
  if (file_size < 0)
    return ScanResult::kUnreadable;
  size = uint64_t(file_size);
  // Not a vector: its zero fill would write every byte once more before the read does.
  uint8_t *mem = static_cast<uint8_t *>(std::malloc(size_t(std::max<uint64_t>(size, 1))));
  if (!mem)
    return ScanResult::kNoMemory;
  std::shared_ptr<const uint8_t> buf(mem, [](const uint8_t *p) { std::free(const_cast<uint8_t *>(p)); });
  // In 32 MB reads (one request per read to the file system service), the whole file before another card operation:
  // the card reads it front to back.
  {
    std::lock_guard<NxMutex> lock(CardMutex());  // one card operation at a time (nx_file.h)
    for (uint64_t at = 0; at < size; at += kScanChunk) {
      if (cancel && cancel->load(std::memory_order_relaxed))
        return ScanResult::kCancelled;
      const size_t n = size_t(std::min<uint64_t>(kScanChunk, size - at));
      if (!file.f.Read(int64_t(at), mem + at, n))
        return ScanResult::kUnreadable;
    }
  }
  data = std::move(buf);
  return ScanResult::kOk;
}

// tw2de (2026-11-10): file[at, at + n) into the caller's memory, in 32 MB reads, the card's lock taken for each
// read only (the stores write their batches between them).
ScanResult LoadPiece(const std::string &path, uint64_t at, uint8_t *mem, size_t n, const std::atomic<bool> *cancel) {
  NxFile f;
  {
    std::lock_guard<NxMutex> lock(CardMutex());
    if (!f.OpenRead(path))
      return ScanResult::kUnreadable;
  }
  for (size_t done = 0; done < n; done += size_t(kScanChunk)) {
    if (cancel && cancel->load(std::memory_order_relaxed))
      return ScanResult::kCancelled;
    const size_t k = std::min<size_t>(size_t(kScanChunk), n - done);
    std::lock_guard<NxMutex> lock(CardMutex());
    if (!f.Read(int64_t(at + done), mem + done, k))
      return ScanResult::kUnreadable;
  }
  return ScanResult::kOk;
}

bool ScanPiece(const std::string &path, uint64_t file_size, uint64_t piece_at, const uint8_t *data, size_t data_len,
               uint64_t begin, uint64_t end, const ContainerSink &sink, ScanStats &stats,
               const std::atomic<bool> *cancel) {
  end = std::min(end, file_size);
  if (begin >= end)
    return true;
  File file;  // opened only if a stream runs past the piece's bytes
  file.size = file_size;
  file.lazy_path = &path;
  const size_t rel = size_t(begin - piece_at);
  return ScanBuffer(file, begin, data + rel, data_len - rel, size_t(end - begin), sink, stats, cancel);
}

// tw2de (2026-11-10): the whole file into memory the caller holds (the pre-pass's one block, reserved once).
ScanResult LoadWholeFileInto(const std::string &path, uint8_t *mem, uint64_t size, const std::atomic<bool> *cancel) {
  File file;
  if (!file.f.OpenRead(path))
    return ScanResult::kUnreadable;
  const int64_t file_size = file.f.Size();
  if (file_size < 0 || uint64_t(file_size) != size)
    return ScanResult::kUnreadable;
  std::lock_guard<NxMutex> lock(CardMutex());  // one card operation at a time (nx_file.h)
  for (uint64_t at = 0; at < size; at += kScanChunk) {
    if (cancel && cancel->load(std::memory_order_relaxed))
      return ScanResult::kCancelled;
    const size_t n = size_t(std::min<uint64_t>(kScanChunk, size - at));
    if (!file.f.Read(int64_t(at), mem + at, n))
      return ScanResult::kUnreadable;
  }
  return ScanResult::kOk;
}

bool ScanLoaded(const uint8_t *data, uint64_t size, uint64_t begin, uint64_t end, const ContainerSink &sink,
                ScanStats &stats, const std::atomic<bool> *cancel) {
  end = std::min(end, size);
  if (begin >= end)
    return true;
  File none;  // the bytes after the range are all in memory: a stream never needs the file
  none.size = size;
  return ScanBuffer(none, begin, data + begin, size_t(size - begin), size_t(end - begin), sink, stats, cancel);
}

}  // namespace renut::nxs

// tw2de (2026-10-21): the Xbox 360 shader containers in the game's files, found on the console (the C++ port of
// tools/tw2de_scan_shaders.py, its contiguous containers: all 18,791 the PC scan found were). Every 0x102A110x header
// in the files' raw bytes and in every zlib stream in them whose physical part follows it as well-formed microcode.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace renut::nxs {

struct ScanStats {
  std::atomic<uint64_t> bytes{0};         // of the file scanned so far (raw bytes, not the streams' output)
  std::atomic<uint64_t> streams{0};       // zlib streams inflated
  std::atomic<uint64_t> headers{0};       // well-formed headers
  std::atomic<uint64_t> containers{0};    // headers with their physical part next to them
};

// The part of a file one job scans: the headers and zlib streams starting in [begin, end).
constexpr uint64_t kScanChunk = 32ull << 20;

// Called for each container found (big-endian, virtual part then physical part); from the scanning thread.
using ContainerSink = std::function<void(const uint8_t *container, size_t size)>;

// Scans path[begin, end). False when the file cannot be read. 'cancel' set stops it early (returns false).
bool ScanFileRange(const std::string &path, uint64_t begin, uint64_t end, const ContainerSink &sink, ScanStats &stats,
                   const std::atomic<bool> *cancel = nullptr);

// tw2de (2026-10-21): a whole file in memory (one card operation, 32 MB reads), then its 32 MB ranges scanned
// there by every thread (a stream is inflated from memory to its end: nothing is read twice). kNoMemory: no block
// that large (nothing read; scan it with ScanFileRange instead).
enum class ScanResult { kOk, kUnreadable, kNoMemory, kCancelled };
ScanResult LoadWholeFile(const std::string &path, std::shared_ptr<const uint8_t> &data, uint64_t &size,
                         const std::atomic<bool> *cancel = nullptr);
ScanResult LoadWholeFileInto(const std::string &path, uint8_t *mem, uint64_t size,
                             const std::atomic<bool> *cancel = nullptr);
// tw2de (2026-11-10): a piece of a file in memory. LoadPiece reads file[at, at + n). ScanPiece scans
// [begin, end) of the file with data = file[piece_at, piece_at + data_len) in memory: data_len should reach past the
// piece's end by a margin (a raw container's 1.1 MB at most); a stream past data_len goes on from the file.
ScanResult LoadPiece(const std::string &path, uint64_t at, uint8_t *mem, size_t n,
                     const std::atomic<bool> *cancel = nullptr);
bool ScanPiece(const std::string &path, uint64_t file_size, uint64_t piece_at, const uint8_t *data, size_t data_len,
               uint64_t begin, uint64_t end, const ContainerSink &sink, ScanStats &stats,
               const std::atomic<bool> *cancel = nullptr);
bool ScanLoaded(const uint8_t *data, uint64_t size, uint64_t begin, uint64_t end, const ContainerSink &sink,
                ScanStats &stats, const std::atomic<bool> *cancel = nullptr);

}  // namespace renut::nxs

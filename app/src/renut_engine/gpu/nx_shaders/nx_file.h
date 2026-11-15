// tw2de (2026-10-21): files through libnx's file system calls on the console (FsFile, FsDir: no newlib FILE, no
// devoptab, nothing buffered behind our back); POSIX / std::filesystem elsewhere (the PC tests of this code).
// Paths are "sdmc:/..." (or any fsdev device's) on the console.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "renut_engine/gpu/nx_shaders/nx_lock.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace renut::nxs {

class NxFile {
 public:
  NxFile() = default;
  NxFile(const NxFile &) = delete;
  NxFile &operator=(const NxFile &) = delete;
  ~NxFile() { Close(); }

  // Read and write; create_size >= 0 creates a missing file that long (fsFsCreateFile: the space is the card's,
  // nothing is written).
  bool Open(const std::string &path, int64_t create_size = -1);
  bool OpenRead(const std::string &path);
  void Close();
  bool IsOpen() const { return open_; }

  int64_t Size();
  bool SetSize(int64_t size);  // fsFileSetSize: no data written
  bool Read(int64_t at, void *dst, size_t size);  // all of it, or false
  size_t ReadSome(int64_t at, void *dst, size_t size);  // what there is (to the end of the file)
  bool Write(int64_t at, const void *src, size_t size);
  bool Flush();  // to the card

  static bool Delete(const std::string &path);

 private:
  bool open_ = false;
#if defined(__SWITCH__)
  FsFile file_{};
#else
  int fd_ = -1;
#endif
};

// tw2de (2026-10-21): one operation on the card at a time (the pre-pass's game file reads, the shader store's and
// the DKSH cache's batch writes): the card reads or writes one place front to back instead of seeking between threads.
NxMutex &CardMutex();

struct NxDirEntry {
  std::string path;  // as the root was given, then "/name/..."
  uint64_t size;
};

// Every file under root whose name ends in suffix (case-insensitive; empty: all), recursively, sorted by path.
// The console: fsFsOpenDirectory + fsDirRead (name, type and size in one go, in batches).
std::vector<NxDirEntry> NxListFiles(const std::string &root, const std::string &suffix);

}  // namespace renut::nxs

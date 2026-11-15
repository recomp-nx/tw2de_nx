// tw2de (2026-10-21): nx_file.h.
#include "renut_engine/gpu/nx_shaders/nx_file.h"
#include "renut_engine/gpu/nx_shaders/nx_lock.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>

#if !defined(__SWITCH__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#endif

namespace renut::nxs {

#if defined(__SWITCH__)
uint64_t WaitTicks() { return armGetSystemTick(); }
double WaitTicksToMs(uint64_t ticks) { return double(ticks) * 1000.0 / double(armGetSystemTickFreq()); }
#else
uint64_t WaitTicks() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
double WaitTicksToMs(uint64_t ticks) { return double(ticks) / 1e6; }
#endif

NxMutex &CardMutex() {
  static NxMutex mutex;
  return mutex;
}

namespace {

bool EndsWithNoCase(const std::string &s, const std::string &suffix) {
  if (suffix.size() > s.size())
    return false;
  for (size_t i = 0; i < suffix.size(); i++)
    if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i])))
      return false;
  return true;
}

}  // namespace

#if defined(__SWITCH__)

namespace {
// "sdmc:/switch/x" -> the device's file system and "/switch/x".
FsFileSystem *DeviceFs(const std::string &path, std::string &fs_path) {
  const size_t colon = path.find(":/");
  fs_path = colon == std::string::npos ? path : path.substr(colon + 1);
  return fsdevGetDeviceFileSystem(colon == std::string::npos ? "sdmc" : path.substr(0, colon).c_str());
}

// tw2de (2026-10-21): the file system service takes a path as a fixed FS_MAX_PATH-byte buffer: the whole buffer is
// sent, so a std::string's c_str() (shorter storage) hands it whatever follows the string in memory, or fails when
// that is not mapped (directories were randomly not listed). Every path goes through one of these, zero-filled.
struct FsPath {
  char buf[FS_MAX_PATH];
  explicit FsPath(const std::string &path) {
    std::memset(buf, 0, sizeof(buf));
    std::strncpy(buf, path.c_str(), sizeof(buf) - 1);
  }
  const char *c_str() const { return buf; }
};
}  // namespace

bool NxFile::Open(const std::string &path, int64_t create_size) {
  Close();
  std::string p;
  FsFileSystem *fs = DeviceFs(path, p);
  if (!fs)
    return false;
  constexpr u32 mode = FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append;  // Append: writes past the end grow it
  const FsPath fp(p);
  if (R_FAILED(fsFsOpenFile(fs, fp.c_str(), mode, &file_))) {
    if (create_size < 0 || R_FAILED(fsFsCreateFile(fs, fp.c_str(), create_size, 0)) ||
        R_FAILED(fsFsOpenFile(fs, fp.c_str(), mode, &file_)))
      return false;
  }
  open_ = true;
  return true;
}

bool NxFile::OpenRead(const std::string &path) {
  Close();
  std::string p;
  FsFileSystem *fs = DeviceFs(path, p);
  const FsPath fp(p);
  open_ = fs && R_SUCCEEDED(fsFsOpenFile(fs, fp.c_str(), FsOpenMode_Read, &file_));
  return open_;
}

void NxFile::Close() {
  if (open_)
    fsFileClose(&file_);
  open_ = false;
}

int64_t NxFile::Size() {
  s64 size = -1;
  return open_ && R_SUCCEEDED(fsFileGetSize(&file_, &size)) ? int64_t(size) : -1;
}

bool NxFile::SetSize(int64_t size) { return open_ && R_SUCCEEDED(fsFileSetSize(&file_, size)); }

size_t NxFile::ReadSome(int64_t at, void *dst, size_t size) {
  u64 got = 0;
  if (!open_ || R_FAILED(fsFileRead(&file_, at, dst, size, FsReadOption_None, &got)))
    return 0;
  return size_t(got);
}

bool NxFile::Read(int64_t at, void *dst, size_t size) { return ReadSome(at, dst, size) == size; }

bool NxFile::Write(int64_t at, const void *src, size_t size) {
  return open_ && R_SUCCEEDED(fsFileWrite(&file_, at, src, size, FsWriteOption_None));
}

bool NxFile::Flush() { return open_ && R_SUCCEEDED(fsFileFlush(&file_)); }

bool NxFile::Delete(const std::string &path) {
  std::string p;
  FsFileSystem *fs = DeviceFs(path, p);
  const FsPath fp(p);
  return fs && R_SUCCEEDED(fsFsDeleteFile(fs, fp.c_str()));
}

std::vector<NxDirEntry> NxListFiles(const std::string &root, const std::string &suffix) {
  std::vector<NxDirEntry> out;
  std::string p;
  FsFileSystem *fs = DeviceFs(root, p);
  if (!fs)
    return out;
  const std::string prefix = root.substr(0, root.size() - p.size());  // "sdmc:"
  std::vector<std::string> todo{p};
  std::vector<FsDirectoryEntry> entries(64);
  while (!todo.empty()) {
    const std::string dir = todo.back();
    todo.pop_back();
    FsDir d;
    const FsPath fp(dir);
    if (R_FAILED(fsFsOpenDirectory(fs, fp.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d)))
      continue;
    for (;;) {
      s64 n = 0;
      if (R_FAILED(fsDirRead(&d, &n, entries.size(), entries.data())) || n <= 0)
        break;
      for (s64 i = 0; i < n; i++) {
        const FsDirectoryEntry &e = entries[size_t(i)];
        std::string child = dir;
        if (child.empty() || child.back() != '/')
          child += '/';
        child += e.name;
        if (e.type == FsDirEntryType_Dir)
          todo.push_back(child);
        else if (suffix.empty() || EndsWithNoCase(e.name, suffix))
          out.push_back({prefix + child, uint64_t(e.file_size)});
      }
    }
    fsDirClose(&d);
  }
  std::sort(out.begin(), out.end(), [](const NxDirEntry &a, const NxDirEntry &b) { return a.path < b.path; });
  return out;
}

#else  // POSIX, for the PC tests

bool NxFile::Open(const std::string &path, int64_t create_size) {
  Close();
  fd_ = ::open(path.c_str(), O_RDWR);
  if (fd_ < 0 && create_size >= 0) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ >= 0 && ::ftruncate(fd_, create_size) != 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  open_ = fd_ >= 0;
  return open_;
}

bool NxFile::OpenRead(const std::string &path) {
  Close();
  fd_ = ::open(path.c_str(), O_RDONLY);
  open_ = fd_ >= 0;
  return open_;
}

void NxFile::Close() {
  if (fd_ >= 0)
    ::close(fd_);
  fd_ = -1;
  open_ = false;
}

int64_t NxFile::Size() {
  struct stat st;
  return open_ && ::fstat(fd_, &st) == 0 ? int64_t(st.st_size) : -1;
}

bool NxFile::SetSize(int64_t size) { return open_ && ::ftruncate(fd_, size) == 0; }

size_t NxFile::ReadSome(int64_t at, void *dst, size_t size) {
  size_t got = 0;
  while (open_ && got < size) {
    const ssize_t n = ::pread(fd_, static_cast<char *>(dst) + got, size - got, at + int64_t(got));
    if (n <= 0)
      break;
    got += size_t(n);
  }
  return got;
}

bool NxFile::Read(int64_t at, void *dst, size_t size) { return ReadSome(at, dst, size) == size; }

bool NxFile::Write(int64_t at, const void *src, size_t size) {
  return open_ && ::pwrite(fd_, src, size, at) == ssize_t(size);
}

bool NxFile::Flush() { return open_ && ::fsync(fd_) == 0; }

bool NxFile::Delete(const std::string &path) { return ::unlink(path.c_str()) == 0; }

std::vector<NxDirEntry> NxListFiles(const std::string &root, const std::string &suffix) {
  std::vector<NxDirEntry> out;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code fec;
    if (!it->is_regular_file(fec))
      continue;
    const std::string name = it->path().filename().string();
    if (!suffix.empty() && !EndsWithNoCase(name, suffix))
      continue;
    out.push_back({it->path().string(), uint64_t(it->file_size(fec))});
  }
  std::sort(out.begin(), out.end(), [](const NxDirEntry &a, const NxDirEntry &b) { return a.path < b.path; });
  return out;
}

#endif

}  // namespace renut::nxs

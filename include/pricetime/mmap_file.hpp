#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pricetime {

// Read-only memory map of an entire file.
//
// A full-day ITCH file is several gigabytes. Mapping it instead of copying it
// through a read() loop means the parser walks pages the kernel faults in on
// demand, with no user-space buffer, no copy, and no artificial record
// boundary to stitch messages across. Throughput measurements then reflect
// parsing and book-building rather than memcpy.
class MmapFile {
 public:
  MmapFile() = default;

  explicit MmapFile(const std::string& path) { open(path); }

  MmapFile(const MmapFile&) = delete;
  MmapFile& operator=(const MmapFile&) = delete;

  MmapFile(MmapFile&& other) noexcept { move_from(other); }

  MmapFile& operator=(MmapFile&& other) noexcept {
    if (this != &other) {
      close();
      move_from(other);
    }
    return *this;
  }

  ~MmapFile() { close(); }

  bool open(const std::string& path) {
    close();
#if defined(_WIN32)
    file_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      error_ = "cannot open " + path;
      return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file_, &sz)) {
      error_ = "cannot size " + path;
      close();
      return false;
    }
    size_ = static_cast<std::size_t>(sz.QuadPart);
    if (size_ == 0) {
      error_ = "empty file " + path;
      close();
      return false;
    }
    mapping_ = CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_ == nullptr) {
      error_ = "cannot map " + path;
      close();
      return false;
    }
    data_ = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (data_ == nullptr) {
      error_ = "cannot view " + path;
      close();
      return false;
    }
    return true;
#else
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
      error_ = "cannot open " + path;
      return false;
    }
    struct stat st {};
    if (::fstat(fd_, &st) != 0 || st.st_size <= 0) {
      error_ = "cannot size " + path;
      close();
      return false;
    }
    size_ = static_cast<std::size_t>(st.st_size);
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (p == MAP_FAILED) {
      error_ = "cannot map " + path;
      close();
      return false;
    }
    data_ = static_cast<const std::uint8_t*>(p);
    // The parser walks the file strictly front to back, exactly the access
    // pattern this hint describes; it lets the kernel read ahead aggressively
    // and drop pages behind us.
    ::madvise(const_cast<void*>(p), size_, MADV_SEQUENTIAL);
    return true;
#endif
  }

  void close() {
#if defined(_WIN32)
    if (data_ != nullptr) UnmapViewOfFile(data_);
    if (mapping_ != nullptr) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE && file_ != nullptr) CloseHandle(file_);
    data_ = nullptr;
    mapping_ = nullptr;
    file_ = INVALID_HANDLE_VALUE;
#else
    if (data_ != nullptr) ::munmap(const_cast<std::uint8_t*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
    data_ = nullptr;
    fd_ = -1;
#endif
    size_ = 0;
  }

  [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  void move_from(MmapFile& other) {
    data_ = other.data_;
    size_ = other.size_;
    error_ = other.error_;
    other.data_ = nullptr;
    other.size_ = 0;
#if defined(_WIN32)
    file_ = other.file_;
    mapping_ = other.mapping_;
    other.file_ = INVALID_HANDLE_VALUE;
    other.mapping_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
  }

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::string error_;
#if defined(_WIN32)
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE mapping_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace pricetime

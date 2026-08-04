#pragma once

#include <fcntl.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

class HalFile {
  friend class HalStorage;
  mutable std::fstream stream;
  std::filesystem::path physicalPath;

  explicit HalFile(const std::filesystem::path& path, bool write) : physicalPath(path) {
    if (write) {
      stream.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    } else {
      stream.open(path, std::ios::binary | std::ios::in);
    }
  }

  explicit HalFile(const std::filesystem::path& path, int flags) : physicalPath(path) {
    std::ios::openmode mode = std::ios::binary;
    if (flags & O_RDWR)
      mode |= std::ios::in | std::ios::out;
    else if (flags & O_WRONLY)
      mode |= std::ios::out;
    else
      mode |= std::ios::in;
    if (flags & O_TRUNC) mode |= std::ios::trunc;
    stream.open(path, mode);
  }

 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  uint64_t fileSize64() { return std::filesystem::file_size(physicalPath); }
  bool seek64(uint64_t position) {
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(position), std::ios::beg);
    return static_cast<bool>(stream);
  }
  size_t position() const {
    const auto value = stream.tellg();
    return value < 0 ? 0 : static_cast<size_t>(value);
  }
  bool isOpen() const { return stream.is_open(); }
  int available() const { return stream && stream.peek() != std::char_traits<char>::eof(); }
  int read(void* buffer, size_t count) {
    stream.read(static_cast<char*>(buffer), static_cast<std::streamsize>(count));
    return static_cast<int>(stream.gcount());
  }
  size_t write(const void* buffer, size_t count) {
    stream.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(count));
    return stream ? count : 0;
  }
  size_t write(uint8_t value) { return write(&value, sizeof(value)); }
  bool close() {
    if (stream.is_open()) stream.close();
    return true;
  }
};

class HalStorage {
  std::filesystem::path root;

 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  void setRoot(std::filesystem::path value) { root = std::move(value); }

  std::filesystem::path resolve(const char* logicalPath) const {
    std::string relative = logicalPath ? logicalPath : "";
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    return root / relative;
  }

  bool exists(const char* path) { return std::filesystem::exists(resolve(path)); }
  HalFile open(const char* path, int flags = O_RDONLY) {
    const auto physical = resolve(path);
    std::filesystem::create_directories(physical.parent_path());
    return HalFile(physical, flags);
  }
  bool mkdir(const char* path, bool = true) {
    std::error_code error;
    std::filesystem::create_directories(resolve(path), error);
    return !error;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    file = HalFile(resolve(path.c_str()), false);
    return file.stream.is_open();
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    const auto physical = resolve(path.c_str());
    std::filesystem::create_directories(physical.parent_path());
    file = HalFile(physical, true);
    return file.stream.is_open();
  }
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t = 0) {
    if (bufferSize == 0) return 0;
    std::ifstream input(resolve(path), std::ios::binary);
    if (!input) {
      buffer[0] = '\0';
      return 0;
    }
    input.read(buffer, static_cast<std::streamsize>(bufferSize - 1));
    const size_t count = static_cast<size_t>(input.gcount());
    buffer[count] = '\0';
    return count;
  }
  bool remove(const char* path) {
    std::error_code error;
    const bool removed = std::filesystem::remove(resolve(path), error);
    return removed && !error;
  }
  bool rename(const char* oldPath, const char* newPath) {
    std::error_code error;
    std::filesystem::rename(resolve(oldPath), resolve(newPath), error);
    return !error;
  }
  bool removeDir(const char* path) {
    std::error_code error;
    std::filesystem::remove_all(resolve(path), error);
    return !error;
  }
};

#define Storage HalStorage::getInstance()

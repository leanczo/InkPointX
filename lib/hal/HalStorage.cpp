#include "HalStorage.h"

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cassert>

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() { return SDCard.begin(); }

bool HalStorage::ready() const { return SDCard.ready(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) {
  // Chunked read with an up-front reserve. The SDK's readFile accumulates one
  // byte at a time into an Arduino String, which reallocates every 16 bytes —
  // ~3,000 reallocs and a heap-fragmentation trail for a 50 KB settings file,
  // paid on every store load at boot and every bookmark load at runtime.
  StorageLock lock;
  HalFile f;
  if (!openFileForRead("SD", path, f)) return String();
  constexpr size_t maxSize = 50000;  // preserve the SDK's historical cap
  const size_t size = std::min(static_cast<size_t>(f.fileSize()), maxSize);
  String content;
  if (size == 0) return content;
  if (!content.reserve(size)) {
    LOG_ERR("SD", "readFile: cannot reserve %u bytes for %s", (unsigned)size, path);
    return String();
  }
  char buf[512];
  size_t total = 0;
  while (total < size) {
    const int n = f.read(buf, std::min(sizeof(buf), size - total));
    if (n <= 0) break;
    content.concat(buf, static_cast<unsigned int>(n));
    total += static_cast<size_t>(n);
  }
  return content;
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  StorageLock lock;
  const size_t read = SDCard.readFileToBuffer(path, buffer, bufferSize, maxBytes);
  if (read == 0 && recoverInterruptedWrite(path)) {
    return SDCard.readFileToBuffer(path, buffer, bufferSize, maxBytes);
  }
  return read;
}

bool HalStorage::writeFile(const char* path, const String& content) {
  // Atomic replace, not the SDK's delete-then-write: SDCardManager::writeFile
  // removes the old file before writing the new one, so a power loss or card
  // fault in that window destroyed settings.json / state.json / wifi.json
  // outright. Write the full payload to a sibling temp file first and swap it
  // in with a rename — the same pattern ProgressFile::writeAtomic documents.
  // Worst case after a crash is the previous file intact plus a stale .tmp,
  // or (in the tiny remove-to-rename window) the payload intact in the .tmp.
  StorageLock lock;
  const String tmpPath = String(path) + ".tmp";
  {
    HalFile f;
    if (!openFileForWrite("SD", tmpPath, f)) {
      LOG_ERR("SD", "Atomic write: cannot open temp %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(reinterpret_cast<const uint8_t*>(content.c_str()), content.length());
    if (written != content.length()) {
      LOG_ERR("SD", "Atomic write: short write %u/%u to %s", (unsigned)written, (unsigned)content.length(),
              tmpPath.c_str());
      f.close();
      SDCard.remove(tmpPath.c_str());
      return false;
    }
    f.flush();
    // HalFile closes at scope exit; SdFat must not rename a path with an open handle.
  }
  // SdFat rename does not overwrite, so drop the old file first.
  SDCard.remove(path);
  if (!SDCard.rename(tmpPath.c_str(), path)) {
    LOG_ERR("SD", "Atomic write: rename %s -> %s failed", tmpPath.c_str(), path);
    return false;
  }
  return true;
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in CLAUDE.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::recoverInterruptedWrite(const char* path) {
  // Both atomic writers in this firmware (writeFile here, ProgressFile in the
  // reader) replace a file as remove-then-rename, because SdFat's rename will
  // not overwrite. A power loss inside that two-operation window left the
  // payload complete under "<path>.tmp" and no canonical file at all — and
  // nothing ever looked there again, so a settings file or a reading position
  // was lost outright rather than merely being one save stale. Recover it on
  // the first read that misses. Called only from a failed open, so the extra
  // directory lookup is not on any hot path.
  StorageLock lock;
  if (SDCard.exists(path)) return false;
  const String tmpPath = String(path) + ".tmp";
  if (!SDCard.exists(tmpPath.c_str())) return false;
  if (!SDCard.rename(tmpPath.c_str(), path)) {
    LOG_ERR("SD", "Found %s but could not rename it into place", tmpPath.c_str());
    return false;
  }
  LOG_INF("SD", "Recovered an interrupted write: %s", path);
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  if (!ok && recoverInterruptedWrite(path)) {
    ok = SDCard.openFileForRead(moduleName, path, fsFile);
  }
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
bool HalFile::close() { HAL_FILE_WRAPPED_CALL(close, ); }
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return HalFile(std::make_unique<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }

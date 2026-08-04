#pragma once

#include <atomic>
#include <cstdint>
#include <string>

class OtaUpdater {
 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    STORAGE_ERROR,
    INVALID_FIRMWARE_ERROR,
    FLASH_ERROR,
  };

  enum class Phase : uint8_t { IDLE, DOWNLOADING, VERIFYING, FLASHING };

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);

  void setProgress(size_t processed, size_t total, ProgressCallback onProgress, void* ctx);
  void resetProgress();

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize.load(std::memory_order_acquire); }

  size_t getTotalSize() const { return totalSize.load(std::memory_order_acquire); }

  Phase getPhase() const { return phase.load(std::memory_order_acquire); }

 private:
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  std::string otaDigest;
  size_t otaSize = 0;
  std::atomic<size_t> processedSize{0};
  std::atomic<size_t> totalSize{0};
  int lastProgressPercent = -1;
  size_t lastProgressBytes = 0;
  std::atomic<Phase> phase{Phase::IDLE};
};

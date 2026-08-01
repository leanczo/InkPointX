#include "OtaUpdater.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ReleaseJsonParser.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "FirmwareFlasher.h"
#include "HttpDownloader.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/yokki-vans/inkpointx/releases/latest";
constexpr char otaStagingPath[] = "/.ota_update.bin";
constexpr int NETWORK_ATTEMPTS = 3;
constexpr unsigned long RETRY_DELAY_MS = 750;
constexpr size_t HASH_CHUNK_SIZE = 4096;
constexpr size_t MIN_TLS_FREE_HEAP = 64 * 1024;
constexpr size_t MIN_TLS_LARGEST_BLOCK = 32 * 1024;
constexpr int PROGRESS_STEP_PERCENT = 5;

class WifiPowerSaveGuard {
 public:
  WifiPowerSaveGuard() {
    restore_ = esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
    if (!restore_) LOG_ERR("OTA", "Failed to disable Wi-Fi power save");
  }
  WifiPowerSaveGuard(const WifiPowerSaveGuard&) = delete;
  WifiPowerSaveGuard& operator=(const WifiPowerSaveGuard&) = delete;
  ~WifiPowerSaveGuard() {
    if (restore_ && esp_wifi_set_ps(WIFI_PS_MIN_MODEM) != ESP_OK) {
      LOG_ERR("OTA", "Failed to restore Wi-Fi power save");
    }
  }

 private:
  bool restore_ = false;
};

bool hasTlsHeadroom() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t largestBlock = ESP.getMaxAllocHeap();
  LOG_INF("OTA", "TLS preflight: free=%u largest=%u", static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(largestBlock));
  return freeHeap >= MIN_TLS_FREE_HEAP && largestBlock >= MIN_TLS_LARGEST_BLOCK;
}

void waitBeforeRetry(const int attempt) {
  esp_task_wdt_reset();
  delay(RETRY_DELAY_MS * static_cast<unsigned long>(attempt));
}

bool parseSha256Digest(const char* digest, uint8_t (&expected)[32]) {
  constexpr char prefix[] = "sha256:";
  if (!digest || strncmp(digest, prefix, sizeof(prefix) - 1) != 0 || strlen(digest) != 71) return false;

  const auto hexNibble = [](const char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };

  const char* hex = digest + sizeof(prefix) - 1;
  for (size_t i = 0; i < sizeof(expected); ++i) {
    const int high = hexNibble(hex[i * 2]);
    const int low = hexNibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    expected[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

enum class DigestResult { OK, BAD_EXPECTED_DIGEST, OPEN_FAILED, READ_FAILED, OOM, MISMATCH };

OtaUpdater::OtaUpdaterError mapFlashResult(const firmware_flash::Result result) {
  switch (result) {
    case firmware_flash::Result::OK:
      return OtaUpdater::OK;
    case firmware_flash::Result::OPEN_FAIL:
    case firmware_flash::Result::READ_FAIL:
      return OtaUpdater::STORAGE_ERROR;
    case firmware_flash::Result::TOO_SMALL:
    case firmware_flash::Result::TOO_LARGE:
    case firmware_flash::Result::BAD_MAGIC:
    case firmware_flash::Result::BAD_SEGMENTS:
    case firmware_flash::Result::BAD_CHECKSUM:
    case firmware_flash::Result::BAD_SHA:
    case firmware_flash::Result::BAD_SIZE:
      return OtaUpdater::INVALID_FIRMWARE_ERROR;
    case firmware_flash::Result::OOM:
      return OtaUpdater::OOM_ERROR;
    case firmware_flash::Result::NO_PARTITION:
    case firmware_flash::Result::ERASE_FAIL:
    case firmware_flash::Result::WRITE_FAIL:
    case firmware_flash::Result::OTADATA_FAIL:
      return OtaUpdater::FLASH_ERROR;
  }
  return OtaUpdater::FLASH_ERROR;
}

DigestResult verifyReleaseDigest(const char* path, const char* digest) {
  uint8_t expected[32];
  if (!parseSha256Digest(digest, expected)) return DigestResult::BAD_EXPECTED_DIGEST;

  HalFile file;
  if (!Storage.openFileForRead("OTA", path, file) || !file) return DigestResult::OPEN_FAILED;

  auto buffer = makeUniqueNoThrow<uint8_t[]>(HASH_CHUNK_SIZE);
  if (!buffer) {
    file.close();
    return DigestResult::OOM;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, /*is224=*/0);

  size_t remaining = file.fileSize();
  while (remaining > 0) {
    esp_task_wdt_reset();
    const size_t requested = std::min(HASH_CHUNK_SIZE, remaining);
    const int read = file.read(buffer.get(), requested);
    if (read <= 0 || static_cast<size_t>(read) != requested) {
      mbedtls_sha256_free(&sha);
      file.close();
      return DigestResult::READ_FAILED;
    }
    mbedtls_sha256_update(&sha, buffer.get(), requested);
    remaining -= requested;
  }

  uint8_t actual[32];
  mbedtls_sha256_finish(&sha, actual);
  mbedtls_sha256_free(&sha);
  file.close();
  return memcmp(actual, expected, sizeof(actual)) == 0 ? DigestResult::OK : DigestResult::MISMATCH;
}

struct FlashProgressContext {
  OtaUpdater* updater = nullptr;
  OtaUpdater::ProgressCallback onProgress = nullptr;
  void* callbackContext = nullptr;
};

void onFlashProgress(size_t processed, size_t total, void* context) {
  auto* flashContext = static_cast<FlashProgressContext*>(context);
  if (!flashContext || !flashContext->updater) {
    return;
  }
  flashContext->updater->setProgress(processed, total, flashContext->onProgress, flashContext->callbackContext);
}

bool parseVersion(const char* version, int& major, int& minor, int& patch) {
  major = 0;
  minor = 0;
  patch = 0;

  if (!version) {
    return false;
  }

  while (*version && !std::isdigit(static_cast<unsigned char>(*version))) {
    ++version;
  }

  auto readNumber = [](const char*& p, int& value) {
    if (!p || !std::isdigit(static_cast<unsigned char>(*p))) {
      return false;
    }
    value = 0;
    while (*p && std::isdigit(static_cast<unsigned char>(*p))) {
      value = value * 10 + (*p - '0');
      ++p;
    }
    return true;
  };

  const char* p = version;
  if (!readNumber(p, major)) {
    return false;
  }

  if (*p == '.') {
    ++p;
    readNumber(p, minor);
  }

  if (*p == '.') {
    ++p;
    readNumber(p, patch);
  }

  return true;
}

}  // namespace

void OtaUpdater::resetProgress() {
  lastProgressPercent = -1;
  lastProgressBytes = 0;
}

void OtaUpdater::setProgress(const size_t processed, const size_t total, const ProgressCallback onProgress, void* ctx) {
  processedSize = processed;
  if (total > 0) {
    totalSize = total;
  }

  if (total == 0) {
    if (!onProgress) {
      return;
    }
    if (processed == 0 || processed - lastProgressBytes < (64 * 1024)) {
      return;
    }
    lastProgressBytes = processed;
    onProgress(ctx);
    return;
  }

  lastProgressBytes = processed;

  if (lastProgressPercent < 0) {
    lastProgressPercent = 0;
    if (onProgress) {
      onProgress(ctx);
    }
    return;
  }

  const int pct = static_cast<int>(std::min<size_t>(100, (processed * 100) / total));
  if (pct == lastProgressPercent ||
      (pct != 100 && lastProgressPercent >= 0 && pct - lastProgressPercent < PROGRESS_STEP_PERCENT)) {
    return;
  }

  lastProgressPercent = pct;
  if (onProgress) {
    onProgress(ctx);
  }
}

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaDigest.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;
  phase = Phase::IDLE;

  WifiPowerSaveGuard powerSaveGuard;
  OtaUpdaterError lastError = HTTP_ERROR;

  // Stream the release JSON directly into the parser to avoid a second
  // response-sized allocation while TLS is active. A transient DNS/TLS/API
  // failure gets three fresh clients before we report a network error.
  for (int attempt = 1; attempt <= NETWORK_ATTEMPTS; ++attempt) {
    if (!hasTlsHeadroom()) {
      LOG_ERR("OTA", "Not enough contiguous heap for release check");
      return OOM_ERROR;
    }

    ReleaseJsonParser releaseParser;
    const bool fetched =
        HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, const size_t len) {
          releaseParser.feed(reinterpret_cast<const char*>(data), len);
          return true;
        });
    if (!fetched) {
      LOG_ERR("OTA", "Release check attempt %d/%d failed", attempt, NETWORK_ATTEMPTS);
      lastError = HTTP_ERROR;
    } else if (!releaseParser.foundTag()) {
      LOG_ERR("OTA", "Release response has no tag_name (attempt %d/%d)", attempt, NETWORK_ATTEMPTS);
      lastError = JSON_PARSE_ERROR;
    } else if (!releaseParser.foundFirmware()) {
      // A tag becomes the latest release just before CI finishes uploading
      // all assets. Retry this short publication window instead of telling a
      // device that no update exists.
      LOG_ERR("OTA", "Release has no firmware.bin asset (attempt %d/%d)", attempt, NETWORK_ATTEMPTS);
      lastError = NO_UPDATE;
    } else {
      uint8_t expectedDigest[32];
      const char* firmwareUrl = releaseParser.getFirmwareUrl();
      const char* firmwareDigest = releaseParser.getFirmwareDigest();
      const size_t firmwareSize = releaseParser.getFirmwareSize();
      if (!firmwareUrl[0] || firmwareSize == 0 || !parseSha256Digest(firmwareDigest, expectedDigest)) {
        LOG_ERR("OTA", "Release firmware metadata is incomplete (url=%s size=%u digest=%s)",
                firmwareUrl[0] ? "yes" : "no", static_cast<unsigned>(firmwareSize),
                firmwareDigest[0] ? "invalid" : "missing");
        lastError = JSON_PARSE_ERROR;
      } else {
        latestVersion = releaseParser.getTagName();
        otaUrl = firmwareUrl;
        otaDigest = firmwareDigest;
        otaSize = firmwareSize;
        totalSize = otaSize;
        updateAvailable = true;

        LOG_INF("OTA", "Found update: tag=%s size=%u digest=%s", latestVersion.c_str(), static_cast<unsigned>(otaSize),
                otaDigest.c_str());
        return OK;
      }
    }

    if (attempt < NETWORK_ATTEMPTS) waitBeforeRetry(attempt);
  }

  return lastError;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  if (!parseVersion(currentVersion, currentMajor, currentMinor, currentPatch) ||
      !parseVersion(latestVersion.c_str(), latestMajor, latestMinor, latestPatch)) {
    // If version strings are not in expected semver format, fall back to strict string comparison.
    return latestVersion > currentVersion;
  }

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  const auto removeStagingFile = []() {
    if (Storage.exists(otaStagingPath) && !Storage.remove(otaStagingPath)) {
      LOG_ERR("OTA", "Failed to remove staging file");
    }
  };

  if (!Storage.ready()) {
    LOG_ERR("OTA", "SD storage is required for crash-safe OTA");
    return STORAGE_ERROR;
  }

  removeStagingFile();
  WifiPowerSaveGuard powerSaveGuard;

  phase = Phase::DOWNLOADING;
  HttpDownloader::DownloadError downloadResult = HttpDownloader::HTTP_ERROR;
  for (int attempt = 1; attempt <= NETWORK_ATTEMPTS; ++attempt) {
    if (!hasTlsHeadroom()) {
      removeStagingFile();
      return OOM_ERROR;
    }

    resetProgress();
    // The update screen is already physically visible. Do not invoke the
    // render callback at 0%: rehydrating the just-cleared font cache before
    // the TLS handshake would give back the contiguous heap we freed for it.
    setProgress(0, otaSize, nullptr, nullptr);
    downloadResult = HttpDownloader::downloadToFile(
        otaUrl, otaStagingPath, [this, onProgress, ctx](const size_t downloaded, size_t /*reportedTotal*/) {
          // The GitHub API asset size is trusted only after its release digest
          // validates, but it is stable and avoids a redirected CDN reporting
          // an unknown/chunked total that makes progress jump backwards.
          setProgress(downloaded, otaSize, onProgress, ctx);
        });
    if (downloadResult == HttpDownloader::OK) break;

    LOG_ERR("OTA", "Firmware download attempt %d/%d failed: %d", attempt, NETWORK_ATTEMPTS, downloadResult);
    removeStagingFile();
    if (downloadResult == HttpDownloader::FILE_ERROR) return STORAGE_ERROR;
    if (attempt < NETWORK_ATTEMPTS) waitBeforeRetry(attempt);
  }
  if (downloadResult != HttpDownloader::OK) return HTTP_ERROR;

  HalFile stagedFile;
  if (!Storage.openFileForRead("OTA", otaStagingPath, stagedFile) || !stagedFile) {
    removeStagingFile();
    return STORAGE_ERROR;
  }
  const size_t stagedSize = stagedFile.fileSize();
  stagedFile.close();
  if (stagedSize != otaSize) {
    LOG_ERR("OTA", "Downloaded size mismatch: got=%u expected=%u", static_cast<unsigned>(stagedSize),
            static_cast<unsigned>(otaSize));
    removeStagingFile();
    return INVALID_FIRMWARE_ERROR;
  }

  phase = Phase::VERIFYING;
  resetProgress();
  setProgress(0, otaSize, onProgress, ctx);
  const DigestResult digestResult = verifyReleaseDigest(otaStagingPath, otaDigest.c_str());
  if (digestResult != DigestResult::OK) {
    LOG_ERR("OTA", "Release SHA-256 verification failed: %d", static_cast<int>(digestResult));
    removeStagingFile();
    if (digestResult == DigestResult::OOM) return OOM_ERROR;
    if (digestResult == DigestResult::OPEN_FAILED || digestResult == DigestResult::READ_FAILED) return STORAGE_ERROR;
    return INVALID_FIRMWARE_ERROR;
  }
  setProgress(otaSize, otaSize, onProgress, ctx);

  phase = Phase::FLASHING;
  resetProgress();
  setProgress(0, otaSize, onProgress, ctx);
  FlashProgressContext flashContext{this, onProgress, ctx};
  const firmware_flash::Result flashResult =
      firmware_flash::flashFromSdPath(otaStagingPath, onFlashProgress, &flashContext);
  if (flashResult != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Firmware flash failed: %s", firmware_flash::resultName(flashResult));
    removeStagingFile();
    return mapFlashResult(flashResult);
  }

  removeStagingFile();
  setProgress(otaSize, otaSize, onProgress, ctx);
  phase = Phase::IDLE;
  LOG_INF("OTA", "Staged OTA verified and installed successfully");
  return OK;
}

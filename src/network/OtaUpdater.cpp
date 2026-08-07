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
#include <limits>
#include <string>
#include <string_view>
#include <utility>

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

void trimAscii(std::string& value) {
  const auto whitespace = [](const unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; };
  size_t first = 0;
  while (first < value.size() && whitespace(static_cast<unsigned char>(value[first]))) ++first;
  size_t last = value.size();
  while (last > first && whitespace(static_cast<unsigned char>(value[last - 1]))) --last;
  value = value.substr(first, last - first);
}

void removeIncompleteUtf8Tail(std::string& value) {
  if (value.empty()) return;
  size_t start = value.size() - 1;
  while (start > 0 && (static_cast<unsigned char>(value[start]) & 0xC0) == 0x80) --start;
  const unsigned char lead = static_cast<unsigned char>(value[start]);
  size_t expected = 1;
  if ((lead & 0xE0) == 0xC0)
    expected = 2;
  else if ((lead & 0xF0) == 0xE0)
    expected = 3;
  else if ((lead & 0xF8) == 0xF0)
    expected = 4;
  if (value.size() - start < expected) value.resize(start);
}

std::string plainMarkdownLine(std::string line) {
  trimAscii(line);
  while (!line.empty() && line.front() == '#') line.erase(line.begin());
  trimAscii(line);

  bool bullet = false;
  if (line.size() > 1 && (line[0] == '*' || line[0] == '-' || line[0] == '+') && line[1] == ' ') {
    bullet = true;
    line.erase(0, 2);
  } else {
    size_t numberEnd = 0;
    while (numberEnd < line.size() && std::isdigit(static_cast<unsigned char>(line[numberEnd]))) ++numberEnd;
    if (numberEnd > 0 && numberEnd + 1 < line.size() && line[numberEnd] == '.' && line[numberEnd + 1] == ' ') {
      bullet = true;
      line.erase(0, numberEnd + 2);
    }
  }

  // Keep the readable label of Markdown links but omit their usually very
  // long URL. Release links remain available on GitHub itself.
  for (size_t open = line.find('['); open != std::string::npos;) {
    const size_t close = line.find("](", open + 1);
    const size_t end = close == std::string::npos ? std::string::npos : line.find(')', close + 2);
    if (close == std::string::npos || end == std::string::npos) break;
    line.replace(open, end - open + 1, line.substr(open + 1, close - open - 1));
    open = line.find('[', open + 1);
  }

  bool inTag = false;
  std::string clean;
  clean.reserve(line.size() + 2);
  for (const char c : line) {
    if (c == '<') {
      inTag = true;
      continue;
    }
    if (c == '>' && inTag) {
      inTag = false;
      continue;
    }
    if (!inTag && c != '*' && c != '`') clean.push_back(c);
  }
  trimAscii(clean);
  return bullet && !clean.empty() ? std::string("- ") + clean : clean;
}

std::string sanitizeReleaseNotes(std::string notes) {
  removeIncompleteUtf8Tail(notes);
  std::string result;
  result.reserve(notes.size());
  size_t position = 0;
  bool previousBlank = true;
  while (position <= notes.size()) {
    const size_t end = notes.find('\n', position);
    std::string line = notes.substr(position, end == std::string::npos ? std::string::npos : end - position);
    line = plainMarkdownLine(std::move(line));

    // GitHub's generated footer is a raw comparison URL and adds no useful
    // information on the reader's compact screen.
    const bool generatedFooter = line.rfind("Full Changelog", 0) == 0 || line.rfind("https://", 0) == 0;
    if (!generatedFooter && (!line.empty() || !previousBlank)) {
      if (!result.empty()) result.push_back('\n');
      result += line;
      previousBlank = line.empty();
    }
    if (end == std::string::npos) break;
    position = end + 1;
  }
  while (!result.empty() && result.back() == '\n') result.pop_back();
  return result;
}

class WifiPowerSaveGuard {
 public:
  WifiPowerSaveGuard() {
    restore_ = esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
    if (!restore_) {
      LOG_ERR("OTA", "Failed to disable Wi-Fi power save");
    }
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
    case firmware_flash::Result::VERIFY_READ_FAIL:
    case firmware_flash::Result::VERIFY_MISMATCH:
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

struct SemanticVersion {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t patch = 0;
  std::string_view prerelease;
};

bool parseVersionNumber(const char*& p, uint32_t& value) {
  if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
  const char* start = p;
  uint64_t parsed = 0;
  while (std::isdigit(static_cast<unsigned char>(*p))) {
    parsed = parsed * 10 + static_cast<unsigned>(*p - '0');
    if (parsed > std::numeric_limits<uint32_t>::max()) return false;
    ++p;
  }
  if (p - start > 1 && *start == '0') return false;
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool validIdentifiers(const std::string_view value, const bool prerelease) {
  if (value.empty()) return false;
  size_t start = 0;
  while (start < value.size()) {
    const size_t end = value.find('.', start);
    const size_t stop = end == std::string_view::npos ? value.size() : end;
    if (stop == start) return false;
    bool numeric = true;
    for (size_t i = start; i < stop; ++i) {
      const unsigned char c = static_cast<unsigned char>(value[i]);
      if (!(std::isalnum(c) || c == '-')) return false;
      if (!std::isdigit(c)) numeric = false;
    }
    if (prerelease && numeric && stop - start > 1 && value[start] == '0') return false;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return true;
}

bool parseVersion(const char* text, SemanticVersion& out) {
  if (!text || !*text) return false;
  const char* p = text;
  if (*p == 'v' || *p == 'V') ++p;
  if (!parseVersionNumber(p, out.major) || *p++ != '.' || !parseVersionNumber(p, out.minor) || *p++ != '.' ||
      !parseVersionNumber(p, out.patch)) {
    return false;
  }

  if (*p == '-') {
    const char* start = ++p;
    while (*p && *p != '+') ++p;
    out.prerelease = std::string_view(start, static_cast<size_t>(p - start));
    if (!validIdentifiers(out.prerelease, true)) return false;
  }
  if (*p == '+') {
    const char* start = ++p;
    while (*p) ++p;
    if (!validIdentifiers(std::string_view(start, static_cast<size_t>(p - start)), false)) return false;
  }
  return *p == '\0';
}

int comparePrerelease(const std::string_view lhs, const std::string_view rhs) {
  if (lhs.empty() || rhs.empty()) {
    if (lhs.empty() == rhs.empty()) return 0;
    return lhs.empty() ? 1 : -1;  // A stable version outranks a prerelease.
  }

  size_t leftPos = 0;
  size_t rightPos = 0;
  while (leftPos < lhs.size() || rightPos < rhs.size()) {
    if (leftPos >= lhs.size()) return -1;
    if (rightPos >= rhs.size()) return 1;
    const size_t leftEnd = lhs.find('.', leftPos);
    const size_t rightEnd = rhs.find('.', rightPos);
    const auto left = lhs.substr(leftPos, (leftEnd == std::string_view::npos ? lhs.size() : leftEnd) - leftPos);
    const auto right = rhs.substr(rightPos, (rightEnd == std::string_view::npos ? rhs.size() : rightEnd) - rightPos);
    const bool leftNumeric =
        std::all_of(left.begin(), left.end(), [](const char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    const bool rightNumeric = std::all_of(right.begin(), right.end(),
                                          [](const char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    if (leftNumeric != rightNumeric) return leftNumeric ? -1 : 1;
    if (leftNumeric) {
      if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    }
    const int idComparison = left.compare(right);
    if (idComparison != 0) return idComparison < 0 ? -1 : 1;
    leftPos = leftEnd == std::string_view::npos ? lhs.size() : leftEnd + 1;
    rightPos = rightEnd == std::string_view::npos ? rhs.size() : rightEnd + 1;
  }
  return 0;
}

int compareVersions(const SemanticVersion& lhs, const SemanticVersion& rhs) {
  if (lhs.major != rhs.major) return lhs.major < rhs.major ? -1 : 1;
  if (lhs.minor != rhs.minor) return lhs.minor < rhs.minor ? -1 : 1;
  if (lhs.patch != rhs.patch) return lhs.patch < rhs.patch ? -1 : 1;
  return comparePrerelease(lhs.prerelease, rhs.prerelease);
}

}  // namespace

void OtaUpdater::resetProgress() {
  lastProgressPercent = -1;
  lastProgressBytes = 0;
}

void OtaUpdater::setProgress(const size_t processed, const size_t total, const ProgressCallback onProgress, void* ctx) {
  if (total > 0) {
    totalSize.store(total, std::memory_order_release);
  }
  processedSize.store(processed, std::memory_order_release);

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
  std::string().swap(releaseNotes);
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
        releaseNotes = sanitizeReleaseNotes(releaseParser.getReleaseNotes());
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

  SemanticVersion current;
  SemanticVersion latest;
  if (!parseVersion(CROSSPOINT_VERSION, current) || !parseVersion(latestVersion.c_str(), latest)) {
    // Never install an ambiguously ordered tag. A malformed GitHub release is
    // a publication error, not a reason to replace working firmware.
    LOG_ERR("OTA", "Refusing malformed version comparison: current=%s latest=%s", CROSSPOINT_VERSION,
            latestVersion.c_str());
    return false;
  }
  return compareVersions(latest, current) > 0;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
const std::string& OtaUpdater::getReleaseNotes() const { return releaseNotes; }
void OtaUpdater::discardReleaseNotes() { std::string().swap(releaseNotes); }

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

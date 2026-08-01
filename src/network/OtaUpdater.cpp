#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen before esp_http_client (which includes lwip). Pin this
// order; clang-format would otherwise sort the local header last and break the
// build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include "FirmwareFlasher.h"
// clang-format on

#include <cctype>
#include <string>

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/yokki-vans/inkpointx/releases/latest";
constexpr char otaStagingPath[] = "/.ota_update.bin";

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

OtaUpdater::OtaUpdaterError performDirectHttpOta(const std::string& otaUrl, OtaUpdater::ProgressCallback onProgress, void* ctx,
                                                  OtaUpdater* updater) {
  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 60000,
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      // The CN/SAN check stays on: this stream is written straight to the OTA
      // partition with no image signature to fall back on, so accepting any
      // publicly-trusted certificate would let a spoofed host flash the device.
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  http_client_init_cb_t setUa = +[](esp_http_client_handle_t client) -> esp_err_t {
    return esp_http_client_set_header(client, "User-Agent", "InkPoint-ESP32-" CROSSPOINT_VERSION);
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = setUa,
  };

  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));
    return OtaUpdater::INTERNAL_UPDATE_ERROR;
  }

  do {
    // installUpdate() runs on the loop task and blocks it for the whole
    // transfer, and the loop task is the one subscribed to the task watchdog.
    // A 5.9 MB image over a weak link takes longer than the 300 s timeout, so
    // without this feed the watchdog panics mid-flash — the update fails
    // exactly on the connections that need the most patience. The staged SD
    // path already feeds from HttpDownloader and FirmwareFlasher.
    esp_task_wdt_reset();
    esp_err = esp_https_ota_perform(ota_handle);
    if (updater) {
      updater->setProgress(esp_https_ota_get_image_len_read(ota_handle), updater->getTotalSize(), onProgress, ctx);
    }
    delay(1);
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return OtaUpdater::HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return OtaUpdater::INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));
    return OtaUpdater::INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "OTA update completed (direct)");
  return OtaUpdater::OK;
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

  const int pct = (processed * 100) / total;
  if (pct == lastProgressPercent) {
    return;
  }

  lastProgressPercent = pct;
  if (onProgress) {
    onProgress(ctx);
  }
}

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
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

  resetProgress();
  const auto restorePowerSave = []() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); };
  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
    LOG_ERR("OTA", "Failed to disable Wi-Fi power save for update");
  }

  setProgress(0, totalSize, onProgress, ctx);
  const bool canStage = Storage.ready();

  const auto directUpdate = [&]() {
    const auto result = performDirectHttpOta(otaUrl, onProgress, ctx, this);
    if (result != OtaUpdater::OK) {
      LOG_ERR("OTA", "Direct OTA update failed");
    }
    return result;
  };

  if (!canStage) {
    LOG_DBG("OTA", "Storage not ready, using direct OTA path only");
    const auto directResult = directUpdate();
    restorePowerSave();
    return directResult;
  }

  const auto directResult = directUpdate();
  if (directResult == OtaUpdater::OK) {
    restorePowerSave();
    return directResult;
  }

  LOG_DBG("OTA", "Direct OTA path failed, trying staging path");

  if (Storage.exists(otaStagingPath)) {
    Storage.remove(otaStagingPath);
  }

  auto onDownloadProgress = [this, onProgress, ctx](size_t downloaded, size_t total) {
    setProgress(downloaded, total, onProgress, ctx);
  };

  if (HttpDownloader::downloadToFile(otaUrl, otaStagingPath, onDownloadProgress) != HttpDownloader::OK) {
    LOG_ERR("OTA", "OTA firmware download failed");
    Storage.remove(otaStagingPath);
    LOG_DBG("OTA", "Falling back to direct OTA path");
    const auto directResult = directUpdate();
    restorePowerSave();
    return directResult;
  }

  FlashProgressContext flashContext{this, onProgress, ctx};
  const auto flashResult = firmware_flash::flashFromSdPath(otaStagingPath, onFlashProgress, &flashContext);

  if (flashResult != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Firmware flash failed: %s", firmware_flash::resultName(flashResult));
    Storage.remove(otaStagingPath);
    LOG_DBG("OTA", "Falling back to direct OTA path after staging flash failure");
    const auto directResult = directUpdate();
    restorePowerSave();
    return directResult;
  }

  if (!Storage.remove(otaStagingPath)) {
    LOG_DBG("OTA", "Failed to remove staging OTA file");
  }

  if (onProgress) {
    processedSize = totalSize;
    onProgress(ctx);
  }

  LOG_INF("OTA", "OTA update staging and flash completed");
  restorePowerSave();
  return OK;
}

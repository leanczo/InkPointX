#include "StravaTokenStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include "network/HttpDownloader.h"  // NetworkFileTransaction

namespace {
constexpr const char* kTokenPath = "/apps/strava/token.json";

// Seeds the very first refresh_token before the device has ever persisted
// its own rotated copy to SD. Ignored once /apps/strava/token.json exists -
// from then on the device's own persisted (rotated) token is authoritative.
// Set via platformio.local.ini (gitignored, see .skills/SKILL.md), e.g.:
//   build_flags =
//     ${base.build_flags}
//     -DSTRAVA_INITIAL_REFRESH_TOKEN=\"your_current_refresh_token\"
#ifndef STRAVA_INITIAL_REFRESH_TOKEN
#define STRAVA_INITIAL_REFRESH_TOKEN ""
#endif
}  // namespace

StravaTokenStore& StravaTokenStore::getInstance() {
  static StravaTokenStore instance;
  return instance;
}

bool StravaTokenStore::load() {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/strava");
  Storage.recoverInterruptedWrite(kTokenPath);

  const String input = Storage.readFile(kTokenPath);
  if (input.length() == 0) {
    refreshToken_ = STRAVA_INITIAL_REFRESH_TOKEN;
    accessToken_.clear();
    expiresAtEpoch_ = 0;
    return !refreshToken_.empty();
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, input.c_str());
  if (err) {
    LOG_ERR("STRAVA", "Token file parse failed: %s", err.c_str());
    refreshToken_ = STRAVA_INITIAL_REFRESH_TOKEN;
    accessToken_.clear();
    expiresAtEpoch_ = 0;
    return !refreshToken_.empty();
  }

  bool ok = false;
  refreshToken_ = obfuscation::deobfuscateFromBase64(doc["refresh_token"] | "", &ok);
  if (!ok || refreshToken_.empty()) {
    // Persisted file exists but isn't usable (corrupt, or written before a
    // successful refresh ever completed) - fall back to the compile-time
    // seed rather than getting permanently stuck with no refresh token.
    refreshToken_ = STRAVA_INITIAL_REFRESH_TOKEN;
    accessToken_.clear();
    expiresAtEpoch_ = 0;
    return !refreshToken_.empty();
  }
  accessToken_ = obfuscation::deobfuscateFromBase64(doc["access_token"] | "", &ok);
  expiresAtEpoch_ = doc["expires_at"] | 0;
  return true;
}

bool StravaTokenStore::save(const std::string& refreshToken, const std::string& accessToken,
                            uint32_t expiresAtEpoch) {
  refreshToken_ = refreshToken;
  accessToken_ = accessToken;
  expiresAtEpoch_ = expiresAtEpoch;

  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/strava");

  JsonDocument doc;
  doc["refresh_token"] = obfuscation::obfuscateToBase64(refreshToken);
  doc["access_token"] = obfuscation::obfuscateToBase64(accessToken);
  doc["expires_at"] = expiresAtEpoch;
  String json;
  serializeJson(doc, json);

  const String finalPath = kTokenPath;
  String tempPath;
  if (!NetworkFileTransaction::prepare(finalPath, ".strava-tmp", "STRAVA", tempPath)) {
    LOG_ERR("STRAVA", "Failed to prepare token write");
    return false;
  }
  if (!Storage.writeFile(tempPath.c_str(), json)) {
    Storage.remove(tempPath.c_str());
    LOG_ERR("STRAVA", "Failed to write staged token file");
    return false;
  }
  if (!NetworkFileTransaction::commit(finalPath, tempPath, "STRAVA")) {
    Storage.remove(tempPath.c_str());
    LOG_ERR("STRAVA", "Failed to commit token file");
    return false;
  }
  return true;
}

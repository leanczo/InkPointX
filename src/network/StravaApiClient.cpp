#include "StravaApiClient.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>

#include "HttpDownloader.h"  // NetworkFileTransaction

// This client exists instead of adding POST/Bearer-header support to
// HttpDownloader because HttpDownloader's internal shape (Sink,
// runGet/runGetWolf, esp_http_client_config_t) is built entirely around "GET
// a response body, optionally with Basic auth, stream to a sink." Growing it
// to accept an arbitrary method + headers + body would touch a file every
// other network consumer in this codebase depends on, for the sake of the
// one OAuth-flavored caller Strava is (and, per SCOPE.md, is meant to stay).
// Built directly on esp_http_client + esp_crt_bundle_attach instead - the
// same TLS/CA stack HttpDownloader.cpp's runGet() uses for every other HTTPS
// host in this app - via the manual open()/write()/fetch_headers()/read()
// idiom runGet() already established, just with POST + arbitrary headers
// added locally instead of widening that shared file's surface.
//
// This used to be built on freeink::SecureHttpClient (wolfSSL), enabled via
// -DFREEINK_NET_WOLFSSL. That path never actually worked: wolfSSL_connect()
// fails with ASN_PARSE_E (-140) while parsing Strava's GoDaddy-issued
// certificate chain under this build's constrained wolfSSL config
// (WOLFSSL_SP_SMALL etc), and the parse happens before verification mode is
// even consulted - so setInsecure() (tried as a fix) never avoided it. GoDaddy
// is a standard, widely-trusted root that mbedTLS's bundle (esp_crt_bundle)
// parses fine, so switching TLS backends - not tweaking wolfSSL flags -
// fixes it.

namespace {
constexpr uint32_t kHttpTimeoutMs = 60000;  // mirrors HttpDownloader.cpp's HTTP_TIMEOUT_MS
constexpr int kRxBufSize = 2048;            // headers only; Strava's JSON bodies are read separately below
constexpr int kTxBufSize = 512;
constexpr size_t kReadChunk = 1024;
// Token refresh responses are a handful of fields (~300-400 bytes); this caps
// how much this app will ever buffer in RAM for one, generously.
constexpr size_t kMaxTokenResponseBytes = 4096;

// Mirrors HttpDownloader.cpp's hasTlsHeadroom() - duplicated locally rather
// than exported from HttpDownloader.h, since this is the only other caller
// of the manual esp_http_client open/read idiom and widening that header's
// public surface for one consumer isn't worth it.
constexpr size_t MIN_TLS_FREE_HEAP = 64 * 1024;
constexpr size_t MIN_TLS_LARGEST_BLOCK = 32 * 1024;
bool hasTlsHeadroom() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t largestBlock = ESP.getMaxAllocHeap();
  LOG_INF("STRAVA", "TLS preflight: free=%u largest=%u", (unsigned)freeHeap, (unsigned)largestBlock);
  return freeHeap >= MIN_TLS_FREE_HEAP && largestBlock >= MIN_TLS_LARGEST_BLOCK;
}

// URL-encodes a value for an application/x-www-form-urlencoded POST body.
// Strava's client_id/client_secret/refresh_token are always plain
// alphanumerics in practice, but this removes that assumption at near-zero
// cost rather than relying on it silently.
std::string urlEncode(const std::string& value) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// LOG_ERR compiles to nothing unless ENABLE_SERIAL_LOG is set, and even then
// needs a serial monitor attached to read it. A bounded, stack-only line
// appended straight to SD survives both and needs no USB to inspect later -
// same fix PersonalTrackerActivity::doFetch applies for the same reason.
void writeDebugLine(const char* line) {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/strava");
  HalFile debugFile = Storage.open("/apps/strava/debug.log", O_WRITE | O_CREAT | O_APPEND);
  if (!debugFile) return;
  debugFile.write(line, strlen(line));
  debugFile.close();
}

// Opens the (already-configured) client, optionally writes a request body,
// fetches headers, then streams the response through onData in kReadChunk
// pieces - the same open/write/fetch_headers/read idiom HttpDownloader.cpp's
// runGet() uses, generalized to take POST bodies and custom headers, which is
// exactly the shape HttpDownloader itself deliberately doesn't grow (see the
// comment at the top of this file). No redirect handling: neither the OAuth
// endpoint nor the Strava API redirects.
esp_err_t sendAndReceive(esp_http_client_handle_t client, const uint8_t* body, size_t bodyLen, int& status,
                         const std::function<bool(const uint8_t*, size_t)>& onData) {
  const esp_err_t openResult = esp_http_client_open(client, static_cast<int>(bodyLen));
  if (openResult != ESP_OK) {
    LOG_ERR("STRAVA", "open failed: %s, heap free=%u largest=%u", esp_err_to_name(openResult),
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return openResult;
  }

  if (body != nullptr && bodyLen > 0) {
    const int written = esp_http_client_write(client, reinterpret_cast<const char*>(body), static_cast<int>(bodyLen));
    if (written < 0 || static_cast<size_t>(written) != bodyLen) {
      LOG_ERR("STRAVA", "write failed: %d", written);
      return ESP_FAIL;
    }
  }

  const int64_t contentLength = esp_http_client_fetch_headers(client);
  status = esp_http_client_get_status_code(client);
  if (contentLength < 0 || status <= 0) {
    LOG_ERR("STRAVA", "response header fetch failed");
    return ESP_FAIL;
  }

  char buf[kReadChunk];
  while (true) {
    const int read = esp_http_client_read(client, buf, sizeof(buf));
    if (read < 0) {
      LOG_ERR("STRAVA", "read error, heap free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMaxAllocHeap());
      return ESP_FAIL;
    }
    if (read == 0) break;
    if (onData && !onData(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(read))) return ESP_FAIL;
  }

  return esp_http_client_is_complete_data_received(client) ? ESP_OK : ESP_FAIL;
}
}  // namespace

bool StravaApiClient::refreshAccessToken(const std::string& clientId, const std::string& clientSecret,
                                         const std::string& currentRefreshToken, TokenRefreshResult& out) {
  if (!hasTlsHeadroom()) {
    LOG_ERR("STRAVA", "Skipping token refresh: insufficient TLS heap headroom");
    writeDebugLine("[strava] token refresh skipped: insufficient TLS heap headroom\n");
    return false;
  }

  const std::string payload = "client_id=" + urlEncode(clientId) + "&client_secret=" + urlEncode(clientSecret) +
                              "&grant_type=refresh_token&refresh_token=" + urlEncode(currentRefreshToken);

  esp_http_client_config_t config = {};
  config.url = "https://www.strava.com/oauth/token";
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = kHttpTimeoutMs;
  config.buffer_size = kRxBufSize;
  config.buffer_size_tx = kTxBufSize;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("STRAVA", "Token refresh: client init failed");
    writeDebugLine("[strava] token refresh: client init failed\n");
    return false;
  }
  esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
  esp_http_client_set_header(client, "User-Agent", "InkPoint-ESP32-" CROSSPOINT_VERSION);

  std::string body;
  int status = 0;
  const esp_err_t err = sendAndReceive(
      client, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), status,
      [&body](const uint8_t* data, size_t len) {
        if (body.size() + len > kMaxTokenResponseBytes) return false;
        body.append(reinterpret_cast<const char*>(data), len);
        return true;
      });
  esp_http_client_cleanup(client);

  if (err != ESP_OK || status != 200) {
    LOG_ERR("STRAVA", "Token refresh failed: status=%d err=%s", status, esp_err_to_name(err));
    char buf[300];
    snprintf(buf, sizeof(buf),
            "[%lu] token refresh HTTP status=%d client_id_len=%u refresh_token_len=%u err=%s body=%.60s\n",
            static_cast<unsigned long>(millis()), status, static_cast<unsigned>(clientId.size()),
            static_cast<unsigned>(currentRefreshToken.size()), esp_err_to_name(err), body.c_str());
    writeDebugLine(buf);
    return false;
  }

  // Only keep the three fields this client actually uses - same RAM
  // discipline every other Strava parse follows (see StravaActivity.cpp).
  JsonDocument filter;
  filter["access_token"] = true;
  filter["refresh_token"] = true;
  filter["expires_at"] = true;

  JsonDocument doc;
  const DeserializationError parseErr = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (parseErr) {
    LOG_ERR("STRAVA", "Token refresh JSON parse failed: %s", parseErr.c_str());
    char buf[128];
    snprintf(buf, sizeof(buf), "[%lu] token refresh JSON parse failed: %s\n", static_cast<unsigned long>(millis()),
            parseErr.c_str());
    writeDebugLine(buf);
    return false;
  }

  out.accessToken = doc["access_token"] | "";
  out.refreshToken = doc["refresh_token"] | "";
  out.expiresAtEpoch = doc["expires_at"] | 0;
  if (out.accessToken.empty() || out.refreshToken.empty()) {
    LOG_ERR("STRAVA", "Token refresh response missing tokens");
    writeDebugLine("[strava] token refresh: 200 OK but response missing tokens\n");
    return false;
  }
  char okBuf[80];
  snprintf(okBuf, sizeof(okBuf), "[%lu] token refresh OK, expires_at=%lu\n", static_cast<unsigned long>(millis()),
          static_cast<unsigned long>(out.expiresAtEpoch));
  writeDebugLine(okBuf);
  return true;
}

bool StravaApiClient::authenticatedGetToFile(const std::string& url, const std::string& accessToken,
                                             const std::string& destPath) {
  if (!hasTlsHeadroom()) {
    LOG_ERR("STRAVA", "Skipping GET: insufficient TLS heap headroom: %s", url.c_str());
    writeDebugLine("[strava] GET skipped: insufficient TLS heap headroom\n");
    return false;
  }

  const String finalPath = destPath.c_str();
  String tempPath;
  if (!NetworkFileTransaction::prepare(finalPath, ".strava-tmp", "STRAVA", tempPath)) {
    LOG_ERR("STRAVA", "Failed to prepare download: %s", destPath.c_str());
    writeDebugLine("[strava] GET: failed to prepare staged download file\n");
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite("STRAVA", tempPath.c_str(), file)) {
    Storage.remove(tempPath.c_str());
    LOG_ERR("STRAVA", "Failed to open staged file for writing: %s", tempPath.c_str());
    writeDebugLine("[strava] GET: failed to open staged file for writing\n");
    return false;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = kHttpTimeoutMs;
  config.buffer_size = kRxBufSize;
  config.buffer_size_tx = kTxBufSize;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    file.close();
    Storage.remove(tempPath.c_str());
    LOG_ERR("STRAVA", "GET: client init failed: %s", url.c_str());
    writeDebugLine("[strava] GET: client init failed\n");
    return false;
  }
  const std::string authHeader = "Bearer " + accessToken;
  esp_http_client_set_header(client, "Authorization", authHeader.c_str());
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "User-Agent", "InkPoint-ESP32-" CROSSPOINT_VERSION);

  // Streamed straight to the staged file as chunks arrive, instead of
  // buffering the whole body in RAM first - see the header comment for why
  // (a 30-activity response can run to tens of KB of fields this app doesn't
  // even keep, too large for a second full copy on this device's heap).
  size_t written = 0;
  bool writeFailed = false;
  int status = 0;
  const esp_err_t err = sendAndReceive(client, nullptr, 0, status, [&file, &written, &writeFailed](const uint8_t* data, size_t len) {
    if (file.write(data, len) != len) {
      writeFailed = true;
      return false;
    }
    written += len;
    return true;
  });
  esp_http_client_cleanup(client);

  file.flush();
  file.close();

  const bool ok = err == ESP_OK && status == 200 && !writeFailed && written > 0;
  if (!ok) {
    LOG_ERR("STRAVA", "GET failed: status=%d err=%s written=%u writeFailed=%d url=%s", status, esp_err_to_name(err),
            static_cast<unsigned>(written), writeFailed, url.c_str());
    char buf[256];
    snprintf(buf, sizeof(buf),
            "[%lu] GET %s status=%d err=%s written=%u writeFailed=%d heap_free=%u heap_largest=%u\n",
            static_cast<unsigned long>(millis()), url.c_str(), status, esp_err_to_name(err),
            static_cast<unsigned>(written), writeFailed, static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    writeDebugLine(buf);
    Storage.remove(tempPath.c_str());
    return false;
  }

  if (!NetworkFileTransaction::commit(finalPath, tempPath, "STRAVA")) {
    Storage.remove(tempPath.c_str());
    LOG_ERR("STRAVA", "Failed to commit download: %s", destPath.c_str());
    writeDebugLine("[strava] GET: downloaded OK but failed to commit staged file\n");
    return false;
  }

  char okBuf[128];
  snprintf(okBuf, sizeof(okBuf), "[%lu] GET %s OK, %u bytes\n", static_cast<unsigned long>(millis()), url.c_str(),
          static_cast<unsigned>(written));
  writeDebugLine(okBuf);
  return true;
}

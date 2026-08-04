#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_rom_crc.h>
#include <esp_task_wdt.h>

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#endif

#include <cctype>
#include <cstring>
#include <functional>
#include <string>

namespace NetworkFileTransaction {
namespace {
String hiddenSiblingPath(const String& finalPath, const char* suffix) {
  const int slash = finalPath.lastIndexOf('/');
  String result = finalPath.substring(0, slash + 1);
  result += ".";
  result += finalPath.substring(slash + 1);
  result += suffix;
  return result;
}

bool recoverBackup(const String& finalPath, const String& backupPath, const char* logTag) {
  if (!Storage.exists(backupPath.c_str())) return true;
  if (Storage.exists(finalPath.c_str())) return Storage.remove(backupPath.c_str());
  if (!Storage.rename(backupPath.c_str(), finalPath.c_str())) {
    LOG_ERR(logTag, "Failed to restore transaction backup %s", backupPath.c_str());
    return false;
  }
  return true;
}
}  // namespace

bool prepare(const String& finalPath, const char* tempSuffix, const char* logTag, String& tempPath) {
  const String backupPath = hiddenSiblingPath(finalPath, ".inkpoint-bak");
  if (!recoverBackup(finalPath, backupPath, logTag)) return false;
  tempPath = hiddenSiblingPath(finalPath, tempSuffix);
  if (Storage.exists(tempPath.c_str()) && !Storage.remove(tempPath.c_str())) {
    LOG_ERR(logTag, "Failed to remove stale staged file %s", tempPath.c_str());
    return false;
  }
  return true;
}

bool parkDestination(const String& finalPath, const char* logTag, String& backupPath, bool& existed) {
  backupPath = hiddenSiblingPath(finalPath, ".inkpoint-bak");
  if (!recoverBackup(finalPath, backupPath, logTag)) return false;

  existed = Storage.exists(finalPath.c_str());
  if (existed && !Storage.rename(finalPath.c_str(), backupPath.c_str())) {
    LOG_ERR(logTag, "Failed to preserve destination %s", finalPath.c_str());
    return false;
  }
  return true;
}

void finishParkedDestination(const String& finalPath, const String& backupPath, bool existed, bool commitSucceeded,
                             const char* logTag) {
  if (commitSucceeded) {
    if (existed && !Storage.remove(backupPath.c_str())) {
      LOG_ERR(logTag, "Committed %s but stale backup remains", finalPath.c_str());
    }
    return;
  }

  if (existed && !Storage.rename(backupPath.c_str(), finalPath.c_str())) {
    LOG_ERR(logTag, "CRITICAL: failed to restore %s; backup remains at %s", finalPath.c_str(), backupPath.c_str());
  }
}

bool commit(const String& finalPath, const String& tempPath, const char* logTag) {
  String backupPath;
  bool existed = false;
  if (!parkDestination(finalPath, logTag, backupPath, existed)) return false;
  const bool committed = Storage.rename(tempPath.c_str(), finalPath.c_str());
  finishParkedDestination(finalPath, backupPath, existed, committed, logTag);
  return committed;
}
}  // namespace NetworkFileTransaction

namespace {
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 2048;
// ESP-IDF builds response header keys/values (and especially Location) with
// realloc inside fetch_headers(). With assertions enabled, a late allocation
// failure aborts the whole device instead of returning ESP_ERR_NO_MEM. Hold a
// contiguous block through the TLS handshake, then release it immediately
// before header parsing. If the block or TLS cannot be allocated together we
// fail the request cleanly; after a successful handshake the parser has a
// known reserve for GitHub's redirect and CDN headers.
constexpr size_t RESPONSE_HEADER_RESERVE = 12 * 1024;
constexpr int MAX_REDIRECTS = 5;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

struct UrlOrigin {
  std::string scheme;
  std::string host;
  uint16_t port = 0;
  bool valid = false;
};

UrlOrigin parseOrigin(const std::string& url) {
  UrlOrigin result;
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return result;

  result.scheme = url.substr(0, schemeEnd);
  for (char& c : result.scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (result.scheme != "http" && result.scheme != "https") return result;

  const size_t authorityStart = schemeEnd + 3;
  const size_t authorityEnd = url.find_first_of("/?#", authorityStart);
  std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority.erase(0, at + 1);
  if (authority.empty()) return result;

  std::string portText;
  if (authority.front() == '[') {
    const size_t bracket = authority.find(']');
    if (bracket == std::string::npos) return result;
    result.host = authority.substr(1, bracket - 1);
    if (bracket + 1 < authority.size()) {
      if (authority[bracket + 1] != ':') return result;
      portText = authority.substr(bracket + 2);
    }
  } else {
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      // Unbracketed IPv6 is not a valid HTTP URL authority.
      if (authority.find(':') != colon) return result;
      result.host = authority.substr(0, colon);
      portText = authority.substr(colon + 1);
    } else {
      result.host = authority;
    }
  }
  if (result.host.empty()) return result;
  for (char& c : result.host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  if (portText.empty()) {
    result.port = result.scheme == "https" ? 443 : 80;
  } else {
    unsigned long port = 0;
    for (const char c : portText) {
      if (c < '0' || c > '9') return result;
      port = port * 10 + static_cast<unsigned long>(c - '0');
      if (port > 65535) return result;
    }
    if (port == 0) return result;
    result.port = static_cast<uint16_t>(port);
  }
  result.valid = true;
  return result;
}

bool isSameOrigin(const std::string& first, const std::string& second) {
  const UrlOrigin a = parseOrigin(first);
  const UrlOrigin b = parseOrigin(second);
  return a.valid && b.valid && a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

#if defined(FREEINK_NET_WOLFSSL)
// GitHub's leaf is signed by the long-lived E36 intermediate below. Trusting
// that intermediate directly avoids verifying its P-384 parent on every GET:
// generic P-384 verification is a ~50 KB transient allocation in wolfSSL and
// cannot coexist with the parsed font catalog on an ESP32-C3. ISRG Root X1
// covers GitHub's release-assets host. Keep this deliberately narrow; generic
// HTTPS continues through ESP-IDF's full CA bundle below.
constexpr char GITHUB_ROOT_CAS[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIDXzCCAuagAwIBAgIQNuBZ7YiN1Xrt1XC2cn+b2jAKBggqhkjOPQQDAzBfMQsw
CQYDVQQGEwJHQjEYMBYGA1UEChMPU2VjdGlnbyBMaW1pdGVkMTYwNAYDVQQDEy1T
ZWN0aWdvIFB1YmxpYyBTZXJ2ZXIgQXV0aGVudGljYXRpb24gUm9vdCBFNDYwHhcN
MjEwMzIyMDAwMDAwWhcNMzYwMzIxMjM1OTU5WjBgMQswCQYDVQQGEwJHQjEYMBYG
A1UEChMPU2VjdGlnbyBMaW1pdGVkMTcwNQYDVQQDEy5TZWN0aWdvIFB1YmxpYyBT
ZXJ2ZXIgQXV0aGVudGljYXRpb24gQ0EgRFYgRTM2MFkwEwYHKoZIzj0CAQYIKoZI
zj0DAQcDQgAEaKGnbAUnBYljHDmn/yUhxe3TLxKYuyzc9VXoSaCEV5F73Fhfa/Si
/RMsmwTFW3R9s7J6JpYZFmu4do3vk/Vgl6OCAYEwggF9MB8GA1UdIwQYMBaAFNEi
2kxZ8UtfJjiqndbu6w3D+6lhMB0GA1UdDgQWBBQXmagEwW/kLXCoChA9A9PpGrgm
YzAOBgNVHQ8BAf8EBAMCAYYwEgYDVR0TAQH/BAgwBgEB/wIBADAdBgNVHSUEFjAU
BggrBgEFBQcDAQYIKwYBBQUHAwIwGwYDVR0gBBQwEjAGBgRVHSAAMAgGBmeBDAEC
ATBUBgNVHR8ETTBLMEmgR6BFhkNodHRwOi8vY3JsLnNlY3RpZ28uY29tL1NlY3Rp
Z29QdWJsaWNTZXJ2ZXJBdXRoZW50aWNhdGlvblJvb3RFNDYuY3JsMIGEBggrBgEF
BQcBAQR4MHYwTwYIKwYBBQUHMAKGQ2h0dHA6Ly9jcnQuc2VjdGlnby5jb20vU2Vj
dGlnb1B1YmxpY1NlcnZlckF1dGhlbnRpY2F0aW9uUm9vdEU0Ni5wN2MwIwYIKwYB
BQUHMAGGF2h0dHA6Ly9vY3NwLnNlY3RpZ28uY29tMAoGCCqGSM49BAMDA2cAMGQC
MFsKnBQDh64l+v+aUYWjDCJKQMxHUUGmcwAYDIjJ9pbRYItMCIx5xu0oUb6sIfTX
qQIwPddcsDE4KdeLu1hJdpHgdLvsHAK3vygyLGujMU9xBJCDackRT93VHEE0gppg
NqdV
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICjzCCAhWgAwIBAgIQXIuZxVqUxdJxVt7NiYDMJjAKBggqhkjOPQQDAzCBiDEL
MAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNl
eSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMT
JVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTAwMjAx
MDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMCVVMxEzARBgNVBAgT
Ck5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVUaGUg
VVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBFQ0MgQ2VydGlm
aWNhdGlvbiBBdXRob3JpdHkwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAQarFRaqflo
I+d61SRvU8Za2EurxtW20eZzca7dnNYMYf3boIkDuAUU7FfO7l0/4iGzzvfUinng
o4N+LZfQYcTxmdwlkWOrfzCjtHDix6EznPO/LlxTsV+zfTJ/ijTjeXmjQjBAMB0G
A1UdDgQWBBQ64QmG1M8ZwpZ2dEl23OA1xmNjmjAOBgNVHQ8BAf8EBAMCAQYwDwYD
VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNoADBlAjA2Z6EWCNzklwBBHU6+4WMB
zzuqQhFkoJ2UOQIReVx7Hfpkue4WQrO/isIJxOzksU0CMQDpKmFHjFJKS04YcPbW
RNZu9YO6bVi9JNlWSOrvxKJGgYhqOkbRqZtNyWHa0V1Xahg=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";

bool isGitHubDownloadUrl(const std::string& url) {
  const UrlOrigin origin = parseOrigin(url);
  if (!origin.valid || origin.scheme != "https") return false;
  return origin.host == "github.com" || origin.host == "api.github.com" ||
         origin.host == "release-assets.githubusercontent.com" || origin.host == "objects.githubusercontent.com" ||
         origin.host == "raw.githubusercontent.com";
}

HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password, Sink& sink) {
  std::string url = startUrl;
  bool authorizationActive = !username.empty();
  int transportRetries = 0;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setCACert(GITHUB_ROOT_CAS);
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    http.setUserAgent("InkPoint-ESP32-" CROSSPOINT_VERSION);
    if (authorizationActive) http.setBasicAuth(username, password);

    LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
    const int status = http.GET(
        [&http, &sink](const uint8_t* data, size_t len) {
          if (http.getStatus() != 200) return true;
          if (sink.total == 0 && http.hasContentLength()) sink.total = http.getContentLength();
          if (!sink.write(data, len)) return false;
          sink.downloaded += len;
          // Chunked responses legitimately have no Content-Length. Callers may
          // still know the expected size from their manifest, so keep emitting
          // byte progress with total=0 instead of silencing the callback.
          if (sink.progress) sink.progress(sink.downloaded, sink.total);
          esp_task_wdt_reset();
          return true;
        },
        [&sink]() {
          esp_task_wdt_reset();
          return sink.cancelFlag && *sink.cancelFlag;
        });

    if (http.aborted()) return HttpDownloader::ABORTED;
    if (status < 0) {
      // Certificate verification is the largest transient allocation on the
      // C3. A fragmented heap can reject one handshake even though all of its
      // allocations are released on failure; one fresh client retry then has
      // a contiguous arena and was reliable in device testing. Never retry a
      // request after body bytes were delivered: the sink may be a file, and
      // appending a restarted response to a partial payload corrupts it. The
      // outer file transaction retries such failures from an empty temp file.
      if (sink.downloaded == 0 && transportRetries++ == 0) {
        LOG_INF("HTTP", "wolfSSL transport retry: %s", url.c_str());
        delay(100);
        --hop;  // a failed transport attempt is not a redirect hop
        continue;
      }
      LOG_ERR("HTTP", "wolfSSL request failed: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    transportRetries = 0;
    if (isRedirect(status)) {
      const std::string location = http.getHeader("location");
      std::string nextUrl;
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, nextUrl)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        return HttpDownloader::HTTP_ERROR;
      }
      const UrlOrigin currentOrigin = parseOrigin(url);
      const UrlOrigin nextOrigin = parseOrigin(nextUrl);
      if (!nextOrigin.valid || (currentOrigin.scheme == "https" && nextOrigin.scheme != "https")) {
        LOG_ERR("HTTP", "Refused invalid or insecure redirect");
        return HttpDownloader::HTTP_ERROR;
      }
      if (authorizationActive && !isSameOrigin(url, nextUrl)) {
        authorizationActive = false;
        LOG_DBG("HTTP", "Dropped Authorization on cross-origin redirect");
      }
      url = std::move(nextUrl);
      continue;
    }
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      return HttpDownloader::HTTP_ERROR;
    }
    if (http.callbackAborted()) return HttpDownloader::FILE_ERROR;
    if (!http.responseComplete()) {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  return HttpDownloader::HTTP_ERROR;
}
#endif

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  // Verify HTTPS against the bundled CA roots. This build has esp-tls
  // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
  // up at all; the model is public servers over verified https and local
  // servers over plain http (esp_http_client picks the transport from the URL
  // scheme, so http:// needs no cert config). The prior setInsecure() worked
  // only because Arduino's ssl_client drives mbedtls directly.
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "InkPoint-ESP32-" CROSSPOINT_VERSION);
  bool authorizationActive = !username.empty() && !password.empty();
  if (authorizationActive) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  auto openAndFetchHeaders = [client](int64_t& contentLength, int& status) {
    auto headerReserve = makeUniqueNoThrow<uint8_t[]>(RESPONSE_HEADER_RESERVE);
    if (!headerReserve) {
      LOG_ERR("HTTP", "OOM: %u byte response-header reserve", static_cast<unsigned>(RESPONSE_HEADER_RESERVE));
      return false;
    }

    const esp_err_t openResult = esp_http_client_open(client, 0);
    if (openResult != ESP_OK) {
      LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(openResult));
      return false;
    }

    // See RESPONSE_HEADER_RESERVE above. No OTA progress render can run before
    // fetch_headers() returns, so this contiguous region stays available to
    // the ESP-IDF parser during the exact allocation window that used to abort.
    headerReserve.reset();
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (contentLength < 0 || status <= 0) {
      LOG_ERR("HTTP", "response header fetch failed");
      return false;
    }
    return true;
  };

  int64_t contentLength = 0;
  int status = 0;
  std::string requestUrl = url;
  if (!openAndFetchHeaders(contentLength, status)) {
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  for (int hop = 0; isRedirect(status) && hop < 5; ++hop) {
    // Always close the 30x response before changing URL. This also discards a
    // redirect body instead of asking the next request to reuse a connection
    // with unread bytes.
    esp_http_client_close(client);
    if (esp_http_client_set_redirection(client) != ESP_OK) break;

    // ESP-IDF retains request headers across manual redirects. Basic
    // credentials must never cross an origin boundary (including HTTPS->HTTP
    // or a port change), even when the redirect was returned by a trusted OPDS
    // server.
    char redirectedUrl[1024] = {};
    const bool gotRedirectUrl =
        esp_http_client_get_url(client, redirectedUrl, static_cast<int>(sizeof(redirectedUrl))) == ESP_OK;
    if (authorizationActive && (!gotRedirectUrl || !isSameOrigin(requestUrl, redirectedUrl))) {
      esp_http_client_delete_header(client, "Authorization");
      authorizationActive = false;
      LOG_DBG("HTTP", "Dropped Authorization on cross-origin redirect");
    }
    requestUrl = gotRedirectUrl ? redirectedUrl : std::string();
    if (!openAndFetchHeaders(contentLength, status)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    esp_task_wdt_reset();  // loopTask is on the TWDT; a multi-minute download must feed it
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress) sink.progress(sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink) {
#if defined(FREEINK_NET_WOLFSSL)
  if (isGitHubDownloadUrl(url)) return runGetWolf(url, username, password, sink);
#endif
  return runGet(url, username, password, sink);
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::resolveFirstRedirects(const std::vector<std::string>& urls,
                                           std::vector<std::string>& outUrls) {
#if defined(FREEINK_NET_WOLFSSL)
  if (urls.empty()) {
    outUrls.clear();
    return true;
  }

  // Only batch same-origin GitHub release links. A caller mistake must fall
  // back to the ordinary fully verified download path, not broaden this
  // helper into a generic redirect resolver.
  for (const auto& url : urls) {
    const UrlOrigin origin = parseOrigin(url);
    if (!isGitHubDownloadUrl(url) || !isSameOrigin(urls.front(), url) || origin.host != "github.com") return false;
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setCACert(GITHUB_ROOT_CAS);
    http.setUserAgent("InkPoint-ESP32-" CROSSPOINT_VERSION);

    std::vector<std::string> resolved;
    resolved.reserve(urls.size());
    bool success = true;
    for (const auto& url : urls) {
      if (!http.begin(url)) {
        success = false;
        break;
      }
      LOG_DBG("HTTP", "Resolving release asset: %s", url.c_str());
      const int status = http.GET(
          [](const uint8_t*, size_t) { return true; },
          []() {
            esp_task_wdt_reset();
            return false;
          });
      if (status < 0 || !isRedirect(status) || !http.responseComplete()) {
        success = false;
        break;
      }

      const std::string location = http.getHeader("location");
      std::string nextUrl;
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, nextUrl)) {
        success = false;
        break;
      }
      const UrlOrigin nextOrigin = parseOrigin(nextUrl);
      if (!nextOrigin.valid || nextOrigin.scheme != "https" || !isGitHubDownloadUrl(nextUrl)) {
        success = false;
        break;
      }
      resolved.push_back(std::move(nextUrl));
      esp_task_wdt_reset();
    }

    if (success && resolved.size() == urls.size()) {
      outUrls = std::move(resolved);
      LOG_DBG("HTTP", "Resolved %u release assets over one origin session", static_cast<unsigned>(outUrls.size()));
      return true;
    }
    LOG_INF("HTTP", "Batched redirect resolution failed%s", attempt == 0 ? "; retrying once" : "");
    delay(100);
  }
#else
  (void)urls;
  (void)outUrls;
#endif
  return false;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             uint32_t* outCrc32) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  const String finalPath = destPath.c_str();
  if (Storage.exists(finalPath.c_str())) {
    HalFile existing = Storage.open(finalPath.c_str());
    if (existing && existing.isDirectory()) {
      existing.close();
      LOG_ERR("HTTP", "Download destination is a directory");
      return FILE_ERROR;
    }
    if (existing) existing.close();
  }

  // GitHub release downloads cross two TLS connections and large font assets
  // take long enough for a marginal Wi-Fi link or CDN edge to occasionally
  // drop one. Retry the complete transaction, not the socket read: every
  // attempt gets an empty hidden sibling, so a partial response can never be
  // appended to or promoted over a working font.
#if defined(FREEINK_NET_WOLFSSL)
  const int maxAttempts = isGitHubDownloadUrl(url) ? 4 : 1;
#else
  constexpr int maxAttempts = 1;
#endif
  for (int attempt = 0; attempt < maxAttempts; ++attempt) {
    if (cancelFlag && *cancelFlag) return ABORTED;

    String tempPath;
    if (!NetworkFileTransaction::prepare(finalPath, ".http-tmp", "HTTP", tempPath)) {
      LOG_ERR("HTTP", "Failed to prepare transactional download");
      return FILE_ERROR;
    }

    HalFile file;
    if (!Storage.openFileForWrite("HTTP", tempPath.c_str(), file)) {
      Storage.remove(tempPath.c_str());
      LOG_ERR("HTTP", "Failed to open staged file for writing");
      return FILE_ERROR;
    }

    Sink sink;
    uint32_t attemptCrc32 = 0;
    // Move rather than copy the callback so retry support does not add a
    // second std::function allocation while TLS is competing for C3 heap.
    sink.progress = std::move(progress);
    sink.cancelFlag = cancelFlag;
    sink.write = [&file, &attemptCrc32, outCrc32](const uint8_t* data, size_t len) {
      if (file.write(data, len) != len) return false;
      // Font installation needs a CRC. Computing it while bytes are already
      // hot avoids reopening every file and performing hundreds of extra,
      // mutex-guarded SD reads after the TLS connection closes.
      if (outCrc32) attemptCrc32 = esp_rom_crc32_le(attemptCrc32, data, static_cast<uint32_t>(len));
      return true;
    };

    DownloadError result = runGetSecure(url, username, password, sink);
    progress = std::move(sink.progress);  // retain it for a possible clean retry
    // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
    // otherwise close only after the remove.
    file.flush();
    file.close();

    if (result == OK && sink.downloaded == 0) {
      LOG_ERR("HTTP", "no data received");
      result = HTTP_ERROR;
    }

    if (result == OK) {
      HalFile staged = Storage.open(tempPath.c_str());
      const bool sizeMatches = staged && !staged.isDirectory() && staged.size() == sink.downloaded;
      if (staged) staged.close();
      if (!sizeMatches) {
        LOG_ERR("HTTP", "staged download size mismatch");
        result = FILE_ERROR;
      }
    }

    if (result == OK) {
      if (!NetworkFileTransaction::commit(finalPath, tempPath, "HTTP")) {
        Storage.remove(tempPath.c_str());
        LOG_ERR("HTTP", "Failed to replace destination safely");
        return FILE_ERROR;
      }
      LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
      if (outCrc32) *outCrc32 = attemptCrc32;
      return OK;
    }

    Storage.remove(tempPath.c_str());
    if (result != HTTP_ERROR || attempt + 1 >= maxAttempts || (cancelFlag && *cancelFlag)) return result;

    const unsigned backoffMs = 250u << attempt;
    LOG_INF("HTTP", "Retrying complete download (%d/%d) after %u ms", attempt + 2, maxAttempts, backoffMs);
    const unsigned started = millis();
    while (millis() - started < backoffMs) {
      if (cancelFlag && *cancelFlag) return ABORTED;
      esp_task_wdt_reset();
      delay(25);
    }
  }

  return HTTP_ERROR;
}

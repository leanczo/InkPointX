#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>
#include <vector>

// Shared transactional replacement used by every network writer. The payload
// is written to a hidden sibling first; an existing destination is parked and
// restored if the final rename fails.
namespace NetworkFileTransaction {
bool prepare(const String& finalPath, const char* tempSuffix, const char* logTag, String& tempPath);
bool commit(const String& finalPath, const String& tempPath, const char* logTag);
bool parkDestination(const String& finalPath, const char* logTag, String& backupPath, bool& existed);
void finishParkedDestination(const String& finalPath, const String& backupPath, bool existed, bool commitSucceeded,
                             const char* logTag);
}  // namespace NetworkFileTransaction

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Resolve the first redirect for a batch of download URLs over one reusable
   * connection. Intended for multi-file release downloads: it amortizes the
   * expensive origin TLS handshake while leaving each payload transactional.
   * Returns false without modifying outUrls when batching is unavailable.
   */
  static bool resolveFirstRedirects(const std::vector<std::string>& urls, std::vector<std::string>& outUrls);

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      uint32_t* outCrc32 = nullptr);
};

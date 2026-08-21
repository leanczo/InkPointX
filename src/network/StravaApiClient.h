#pragma once
#include <cstdint>
#include <string>

/**
 * Thin OAuth + Bearer HTTP client for the Strava v3 API, built directly on
 * esp_http_client + esp_crt_bundle_attach (POST + custom headers) rather than
 * on HttpDownloader, which is GET/Basic-auth only and has no POST/header
 * hooks - see the comment at the top of StravaApiClient.cpp for why this
 * isn't folded into HttpDownloader instead.
 */
class StravaApiClient {
 public:
  struct TokenRefreshResult {
    std::string accessToken;
    std::string refreshToken;  // Strava rotates this every refresh - caller MUST persist it
    uint32_t expiresAtEpoch = 0;
  };

  // POST https://www.strava.com/oauth/token, grant_type=refresh_token.
  static bool refreshAccessToken(const std::string& clientId, const std::string& clientSecret,
                                 const std::string& currentRefreshToken, TokenRefreshResult& out);

  // GET with "Authorization: Bearer <accessToken>", streamed straight to
  // destPath (transactional write - hidden temp file, atomic rename only on
  // full success, same guarantee as HttpDownloader::downloadToFile) instead
  // of buffered in RAM. Strava's raw /athlete/activities response carries far
  // more fields than this app keeps (splits, polylines, achievement counts,
  // ...) and can run to tens of KB for a per_page=30 fetch - too large to
  // hold as a second full copy in this device's heap.
  static bool authenticatedGetToFile(const std::string& url, const std::string& accessToken,
                                     const std::string& destPath);
};

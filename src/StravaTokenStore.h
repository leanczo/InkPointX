#pragma once
#include <cstdint>
#include <string>

/**
 * Persists the Strava OAuth refresh/access tokens to SD, obfuscated the same
 * way WifiCredentialStore obfuscates the wifi password (XOR with the
 * device's hardware MAC, then base64 - not cryptographically secure, but
 * keeps them off a casual SD-card read and ties them to this device).
 *
 * Strava rotates the refresh_token on every use, so save() must be called
 * again after every successful refresh, not just once at setup - the
 * compile-time STRAVA_INITIAL_REFRESH_TOKEN seed (see StravaTokenStore.cpp)
 * is only ever used before the device has persisted its own copy.
 */
class StravaTokenStore {
 public:
  static StravaTokenStore& getInstance();

  // Loads the persisted token from SD. If no token file exists yet (or it
  // fails to parse), seeds refreshToken() from STRAVA_INITIAL_REFRESH_TOKEN
  // instead, with accessToken() empty and expiresAtEpoch()==0 so the first
  // use forces an immediate refresh. Returns false only if neither a usable
  // file nor a build-time seed is available.
  bool load();

  // Called after every successful token refresh: persists the new (rotated)
  // refresh_token plus the fresh access_token/expiry so a later launch can
  // skip a redundant refresh if the cached access token hasn't expired yet.
  bool save(const std::string& refreshToken, const std::string& accessToken, uint32_t expiresAtEpoch);

  const std::string& refreshToken() const { return refreshToken_; }
  const std::string& accessToken() const { return accessToken_; }
  uint32_t expiresAtEpoch() const { return expiresAtEpoch_; }
  bool hasRefreshToken() const { return !refreshToken_.empty(); }

 private:
  StravaTokenStore() = default;
  StravaTokenStore(const StravaTokenStore&) = delete;
  StravaTokenStore& operator=(const StravaTokenStore&) = delete;

  std::string refreshToken_;
  std::string accessToken_;
  uint32_t expiresAtEpoch_ = 0;
};

#define STRAVA_TOKEN_STORE StravaTokenStore::getInstance()

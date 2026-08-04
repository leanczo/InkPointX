#include "HalClock.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>

HalClock halClock;  // Singleton instance

namespace {
constexpr time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01 00:00:00 UTC
constexpr time_t MAX_VALID_EPOCH = 4102444800;  // 2100-01-01 00:00:00 UTC
constexpr char CLOCK_STATE_FILE[] = "/.crosspoint/clock_epoch.txt";

#ifndef CROSSPOINT_BUILD_EPOCH
#define CROSSPOINT_BUILD_EPOCH 0
#endif

bool isValidEpoch(const time_t epoch) { return epoch >= MIN_VALID_EPOCH && epoch < MAX_VALID_EPOCH; }

bool setSystemEpoch(const time_t epoch) {
  if (!isValidEpoch(epoch)) return false;
  const timeval value = {.tv_sec = epoch, .tv_usec = 0};
  return settimeofday(&value, nullptr) == 0;
}

// Gregorian civil date to days since 1970-01-01. This avoids changing the
// process-wide TZ merely to interpret the DS3231's UTC calendar registers.
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const int shiftedMonth = static_cast<int>(month) + (month > 2 ? -3 : 9);
  const unsigned dayOfYear = (153 * static_cast<unsigned>(shiftedMonth) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

time_t utcTmToEpoch(const struct tm& value) {
  const int64_t days = daysFromCivil(value.tm_year + 1900, static_cast<unsigned>(value.tm_mon + 1),
                                     static_cast<unsigned>(value.tm_mday));
  return static_cast<time_t>(days * 86400 + value.tm_hour * 3600 + value.tm_min * 60 + value.tm_sec);
}

uint8_t clampUtcOffset(const uint8_t biased) { return biased <= 104 ? biased : 48; }
}  // namespace

void HalClock::begin() {
  _softwareTimeTrusted = isValidEpoch(time(nullptr));
  _available = _sdkRtc.begin();
  if (!_available) {
    LOG_INF("CLK", "Hardware RTC not available");
    return;
  }
  LOG_INF("CLK", "Hardware RTC found");

  // Seed the ESP system clock from the full RTC calendar when it has already
  // been initialised. Older CrossPoint builds wrote only H:M:S, so an invalid
  // calendar is ignored without breaking the existing status-bar clock.
  struct tm rtcTime = {};
  if (readDateTimeFromRTC(rtcTime)) {
    const time_t epoch = utcTmToEpoch(rtcTime);
    if (setSystemEpoch(epoch)) {
      _softwareTimeTrusted = true;
    }
  }
}

bool HalClock::hasValidTime() const {
  if (isValidEpoch(time(nullptr))) return true;
  if (!_available) return false;
  uint8_t hour, minute;
  return getTime(hour, minute);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) {
    const time_t now = time(nullptr);
    if (!isValidEpoch(now)) return false;
    struct tm utc = {};
    gmtime_r(&now, &utc);
    hour = static_cast<uint8_t>(utc.tm_hour);
    minute = static_cast<uint8_t>(utc.tm_min);
    return true;
  }

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime rtcTime;
  if (!_sdkRtc.now(rtcTime)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  _cachedHour = rtcTime.hour;
  _cachedMinute = rtcTime.minute;
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  utcOffsetQuarterHoursBiased = clampUtcOffset(utcOffsetQuarterHoursBiased);
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::getDateTime(struct tm& result, uint8_t utcOffsetQuarterHoursBiased) const {
  time_t utcEpoch = time(nullptr);
  if (!isValidEpoch(utcEpoch)) {
    if (!_available) return false;
    struct tm rtcTime = {};
    if (!readDateTimeFromRTC(rtcTime)) return false;
    utcEpoch = utcTmToEpoch(rtcTime);
    if (!isValidEpoch(utcEpoch)) return false;
  }

  const int offsetQuarterHours = static_cast<int>(clampUtcOffset(utcOffsetQuarterHoursBiased)) - 48;
  const time_t localEpoch = utcEpoch + static_cast<time_t>(offsetQuarterHours) * 15 * 60;
  gmtime_r(&localEpoch, &result);
  return true;
}

bool HalClock::readDateTimeFromRTC(struct tm& result) const {
  if (!_available) return false;

  Rtc::DateTime rtcTime;
  if (!_sdkRtc.now(rtcTime)) return false;

  result = {};
  result.tm_sec = rtcTime.second;
  result.tm_min = rtcTime.minute;
  result.tm_hour = rtcTime.hour;
  result.tm_wday = rtcTime.weekday;
  result.tm_mday = rtcTime.day;
  result.tm_mon = rtcTime.month - 1;
  result.tm_year = rtcTime.year - 1900;

  return result.tm_sec < 60 && result.tm_min < 60 && result.tm_hour < 24 && result.tm_mday >= 1 &&
         result.tm_mday <= 31 && result.tm_mon >= 0 && result.tm_mon < 12;
}

bool HalClock::writeDateTimeToRTC(const struct tm& value) {
  assert(value.tm_hour >= 0 && value.tm_hour < 24);
  assert(value.tm_min >= 0 && value.tm_min < 60);
  assert(value.tm_sec >= 0 && value.tm_sec < 60);
  const Rtc::DateTime rtcTime{static_cast<uint16_t>(value.tm_year + 1900), static_cast<uint8_t>(value.tm_mon + 1),
                              static_cast<uint8_t>(value.tm_mday),         static_cast<uint8_t>(value.tm_hour),
                              static_cast<uint8_t>(value.tm_min),          static_cast<uint8_t>(value.tm_sec),
                              static_cast<uint8_t>(value.tm_wday)};
  if (!_sdkRtc.set(rtcTime)) {
    LOG_ERR("CLK", "Failed to write date/time to hardware RTC");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = static_cast<uint8_t>(value.tm_hour);
  _cachedMinute = static_cast<uint8_t>(value.tm_min);
  _hasCachedTime = true;
  return true;
}

bool HalClock::restoreFromStorage() {
  if (isValidEpoch(time(nullptr))) return true;

  time_t restoredEpoch = 0;
  if (Storage.exists(CLOCK_STATE_FILE)) {
    const String stored = Storage.readFile(CLOCK_STATE_FILE);
    char* end = nullptr;
    const long long parsed = strtoll(stored.c_str(), &end, 10);
    if (end != stored.c_str() && isValidEpoch(static_cast<time_t>(parsed))) {
      restoredEpoch = static_cast<time_t>(parsed);
    }
  }

  if (!isValidEpoch(restoredEpoch)) {
    restoredEpoch = static_cast<time_t>(CROSSPOINT_BUILD_EPOCH + 0);
  }

  if (!setSystemEpoch(restoredEpoch)) {
    LOG_INF("CLK", "No valid software-clock fallback available");
    return false;
  }

  // Restored/build time is intentionally only a visual fallback. X4 has no
  // way to add the time spent fully powered off, so NTP is still requested.
  _softwareTimeTrusted = false;
  LOG_INF("CLK", "Software clock restored from fallback epoch");
  return true;
}

bool HalClock::saveCurrentTime() const {
  const time_t now = time(nullptr);
  if (!isValidEpoch(now)) return false;
  Storage.mkdir("/.crosspoint");
  char epoch[24];
  snprintf(epoch, sizeof(epoch), "%lld", static_cast<long long>(now));
  return Storage.writeFile(CLOCK_STATE_FILE, String(epoch));
}

bool HalClock::syncFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      if (!isValidEpoch(now)) continue;

      if (_available && !writeDateTimeToRTC(timeinfo)) {
        return false;
      }

      _softwareTimeTrusted = true;
      saveCurrentTime();
      LOG_INF("CLK", "%s clock set to %04d-%02d-%02d %02d:%02d:%02d UTC", _available ? "RTC" : "Software",
              timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
              timeinfo.tm_sec);
      return true;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}

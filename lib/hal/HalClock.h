#pragma once

#include <Arduino.h>
#include <Wire.h>

#include <ctime>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;
  bool _softwareTimeTrusted = false;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if the DS3231 RTC is present on this device
  bool isAvailable() const { return _available; }

  // True when either the X3 hardware RTC or the ESP system clock contains a
  // plausible date/time. X4 can obtain a software clock from NTP, but loses it
  // when battery sleep physically removes power from the MCU.
  bool hasValidTime() const;

  // X4 should refresh its software clock once per cold boot. A timestamp
  // restored from SD is useful as a fallback, but is deliberately not treated
  // as current because no clock runs while the X4 is powered off.
  bool needsNetworkSync() const { return !_available && !_softwareTimeTrusted; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if neither a hardware nor software clock is available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if neither a hardware nor software clock is available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Return a complete wall-clock date/time with the requested fixed UTC
  // offset applied. The result uses the conventional struct tm fields.
  bool getDateTime(struct tm& result, uint8_t utcOffsetQuarterHoursBiased = 48) const;

  // Restore the last timestamp saved on the SD card. Call only after Storage
  // is initialised. If no saved timestamp exists, a custom build may supply
  // CROSSPOINT_BUILD_EPOCH as a safe first-boot fallback.
  bool restoreFromStorage();

  // Persist the current system epoch for the next cold boot.
  bool saveCurrentTime() const;

  // Sync the DS3231 RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

 private:
  bool readDateTimeFromRTC(struct tm& result) const;
  bool writeDateTimeToRTC(const struct tm& value);
};

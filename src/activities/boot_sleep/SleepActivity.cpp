#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Txt.h>
#include <WiFi.h>
#include <Xtc.h>

#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "WifiCredentialStore.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

namespace {
constexpr uint8_t UTC_OFFSET_BIAS = 48;
constexpr uint8_t KYIV_STANDARD_OFFSET_Q = UTC_OFFSET_BIAS + 8;  // UTC+2
constexpr uint8_t KYIV_SUMMER_OFFSET_Q = UTC_OFFSET_BIAS + 12;   // UTC+3

int weekdayForDate(int year, int month, int day) {
  // Sakamoto's Gregorian algorithm: 0 = Sunday.
  static constexpr int monthOffsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) --year;
  return (year + year / 4 - year / 100 + year / 400 + monthOffsets[month - 1] + day) % 7;
}

int lastSundayOfMonth(const int year, const int month, const int lastDay) {
  return lastDay - weekdayForDate(year, month, lastDay);
}

uint8_t kyivOffsetForUtc(const struct tm& utc) {
  // Europe/Kyiv follows the European transition rule: daylight time begins
  // at 01:00 UTC on the last Sunday in March and ends at 01:00 UTC on the
  // last Sunday in October.
  const int year = utc.tm_year + 1900;
  const int month = utc.tm_mon + 1;
  if (month < 3 || month > 10) return KYIV_STANDARD_OFFSET_Q;
  if (month > 3 && month < 10) return KYIV_SUMMER_OFFSET_Q;

  const int transitionDay = lastSundayOfMonth(year, month, 31);
  if (month == 3) {
    return (utc.tm_mday > transitionDay || (utc.tm_mday == transitionDay && utc.tm_hour >= 1))
               ? KYIV_SUMMER_OFFSET_Q
               : KYIV_STANDARD_OFFSET_Q;
  }
  return (utc.tm_mday < transitionDay || (utc.tm_mday == transitionDay && utc.tm_hour < 1))
             ? KYIV_SUMMER_OFFSET_Q
             : KYIV_STANDARD_OFFSET_Q;
}

void formatLockDate(const struct tm& local, char* output, const size_t outputSize) {
  static constexpr const char* WEEKDAYS_RU[] = {"Воскресенье", "Понедельник", "Вторник", "Среда",
                                                 "Четверг",     "Пятница",     "Суббота"};
  static constexpr const char* MONTHS_RU[] = {"января",   "февраля", "марта",   "апреля",
                                               "мая",      "июня",    "июля",    "августа",
                                               "сентября", "октября", "ноября",  "декабря"};
  static constexpr const char* WEEKDAYS_UK[] = {"Неділя", "Понеділок", "Вівторок", "Середа",
                                                 "Четвер", "П'ятниця",  "Субота"};
  static constexpr const char* MONTHS_UK[] = {"січня",   "лютого",  "березня", "квітня",
                                               "травня",  "червня", "липня",   "серпня",
                                               "вересня", "жовтня", "листопада", "грудня"};
  static constexpr const char* WEEKDAYS_EN[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                                 "Thursday", "Friday", "Saturday"};
  static constexpr const char* MONTHS_EN[] = {"January", "February", "March",     "April",
                                               "May",     "June",     "July",      "August",
                                               "September", "October", "November", "December"};

  const int weekday = (local.tm_wday >= 0 && local.tm_wday < 7) ? local.tm_wday : 0;
  const int month = (local.tm_mon >= 0 && local.tm_mon < 12) ? local.tm_mon : 0;
  switch (I18N.getLanguage()) {
    case Language::RU:
      snprintf(output, outputSize, "%s, %d %s", WEEKDAYS_RU[weekday], local.tm_mday, MONTHS_RU[month]);
      break;
    case Language::UK:
      snprintf(output, outputSize, "%s, %d %s", WEEKDAYS_UK[weekday], local.tm_mday, MONTHS_UK[month]);
      break;
    default:
      snprintf(output, outputSize, "%s, %s %d", WEEKDAYS_EN[weekday], MONTHS_EN[month], local.tm_mday);
      break;
  }
}
}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::DARK):
      syncClockForSleep();
      return renderLockScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::syncClockForSleep() const {
  if (!halClock.needsNetworkSync()) return;

  bool startedWifi = false;
  if (WiFi.status() != WL_CONNECTED) {
    if (!WIFI_STORE.loadFromFile()) {
      LOG_INF("SLP", "No saved WiFi credentials for sleep-clock sync");
      return;
    }

    const std::string& ssid = WIFI_STORE.getLastConnectedSsid();
    const WifiCredential* credential = ssid.empty() ? nullptr : WIFI_STORE.findCredential(ssid);
    if (!credential) {
      LOG_INF("SLP", "No last-connected WiFi network for sleep-clock sync");
      return;
    }

    LOG_INF("SLP", "Connecting to saved WiFi for sleep-clock sync");
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);
    if (credential->password.empty()) {
      WiFi.begin(credential->ssid.c_str());
    } else {
      WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
    }
    startedWifi = true;

    const unsigned long startedAt = millis();
    constexpr unsigned long CONNECT_TIMEOUT_MS = 5000;
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < CONNECT_TIMEOUT_MS) {
      delay(100);
    }
  }

  if (WiFi.status() == WL_CONNECTED && halClock.syncFromNTP()) {
    SETTINGS.clockHasBeenSynced = 1;
    SETTINGS.saveToFile();
  } else {
    LOG_INF("SLP", "Using saved/build time for sleep screen");
  }

  if (startedWifi) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

void SleepActivity::renderLockScreen() const {
  const int pageWidth = renderer.getScreenWidth();
  char timeText[8] = "--:--";
  char dateText[80] = "Time not set";
  if (I18N.getLanguage() == Language::RU) {
    snprintf(dateText, sizeof(dateText), "Время не задано");
  } else if (I18N.getLanguage() == Language::UK) {
    snprintf(dateText, sizeof(dateText), "Час не встановлено");
  }

  struct tm utc = {};
  struct tm local = {};
  if (halClock.getDateTime(utc, UTC_OFFSET_BIAS) && halClock.getDateTime(local, kyivOffsetForUtc(utc))) {
    snprintf(timeText, sizeof(timeText), "%02d:%02d", local.tm_hour, local.tm_min);
    formatLockDate(local, dateText, sizeof(dateText));
  }

  renderer.clearScreen();

  // iPhone Lock Screen proportions adapted to the X4's 480x800 portrait panel.
  const int centerX = pageWidth / 2;
  constexpr int lockCenterY = 75;
  constexpr int lockRadius = 7;
  renderer.drawArc(lockRadius, centerX, lockCenterY, -1, -1, 2, true);
  renderer.drawArc(lockRadius, centerX, lockCenterY, 1, -1, 2, true);
  renderer.drawLine(centerX - lockRadius, lockCenterY, centerX - lockRadius, lockCenterY + 7, 2, true);
  renderer.drawLine(centerX + lockRadius, lockCenterY, centerX + lockRadius, lockCenterY + 7, 2, true);
  renderer.drawRoundedRect(centerX - 10, lockCenterY + 5, 21, 18, 2, 4, true);

  renderer.drawCenteredText(LOCKSCREEN_DATE_FONT_ID, 130, dateText);
  renderer.drawCenteredText(LOCKSCREEN_CLOCK_FONT_ID, 154, timeText);

  // Draw black-on-white for the normal text pipeline, then invert once so the
  // retained e-ink frame has the same dark appearance as iPhone Always-On.
  renderer.invertScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      if (dir) dir.close();
      return;
    }
    file.close();
  }

  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
      if (dirFile.isDirectory()) {
        dirFile.close();
        continue;
      }
      dirFile.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        dirFile.close();
        continue;
      }

      if (!FsHelpers::hasBmpExtension(filename)) {
        LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
        dirFile.close();
        continue;
      }
      Bitmap bitmap(dirFile);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
        dirFile.close();
        continue;
      }
      files.emplace_back(filename);
      dirFile.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Pick a random wallpaper, excluding recently shown ones.
      // Window: up to SLEEP_RECENT_COUNT entries, capped at numFiles-1.
      const uint16_t fileCount = static_cast<uint16_t>(std::min(numFiles, static_cast<size_t>(UINT16_MAX)));
      const uint8_t window =
          static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), numFiles - 1));
      auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
      for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
        randomFileIndex = static_cast<uint16_t>(random(fileCount));
      }
      APP_STATE.pushRecentSleep(randomFileIndex);
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      HalFile randFile;
      if (Storage.openFileForRead("SLP", filename, randFile)) {
        LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(randFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          randFile.close();
          dir.close();
          return;
        }
        randFile.close();
      }
    }
  }
  if (dir) dir.close();

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  if (hasGreyscale) {
    // OEM grayscale pipeline base: on X3 this displays the frame with the
    // dedicated "AA-pre-BW(mid)" differential waveform, leaving every pixel
    // in the calibrated state the gray nudge refresh expects; on X4 it is a
    // plain HALF refresh (previous behavior).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath) ||
             FsHelpers::hasFb2Extension(APP_STATE.openEpubPath) ||
             FsHelpers::hasPdfExtension(APP_STATE.openEpubPath)) {
    // FB2/PDF packages use the same metadata, cover, and rendering pipeline as EPUB.
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

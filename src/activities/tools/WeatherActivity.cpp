#include "WeatherActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <cctype>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
struct City {
  const char* name;
  double lat;
  double lon;
};

// Curated to the ~20 most recognizable cities worldwide plus Salta -- the
// full ~190-city list made every visit to Select City a long Up/Down grind
// on a 4-button remote for names most users would never pick.
const City CITIES[] = {
    {"Salta", -24.7859, -65.4117},
    {"Buenos Aires", -34.6037, -58.3816},
    {"New York", 40.7128, -74.0060},
    {"Los Angeles", 34.0522, -118.2437},
    {"Mexico City", 19.4326, -99.1332},
    {"Toronto", 43.6532, -79.3832},
    {"Sao Paulo", -23.5505, -46.6333},
    {"London", 51.5074, -0.1278},
    {"Paris", 48.8566, 2.3522},
    {"Madrid", 40.4168, -3.7038},
    {"Rome", 41.9028, 12.4964},
    {"Berlin", 52.5200, 13.4050},
    {"Moscow", 55.7558, 37.6173},
    {"Istanbul", 41.0082, 28.9784},
    {"Dubai", 25.2048, 55.2708},
    {"Cairo", 30.0444, 31.2357},
    {"Tokyo", 35.6762, 139.6503},
    {"Beijing", 39.9042, 116.4074},
    {"Shanghai", 31.2304, 121.4737},
    {"Hong Kong", 22.3193, 114.1694},
    {"Singapore", 1.3521, 103.8198},
    {"Sydney", -33.8688, 151.2093},
};
const int CITY_COUNT = sizeof(CITIES) / sizeof(CITIES[0]);

// Saved configs store a name, not just an index, precisely so a trim like
// this one can't leave a stale index pointing at the wrong city's coordinates
// after a reorder -- name lookup keeps it self-correcting instead.
int findCityIndex(const std::string& name) {
  for (int i = 0; i < CITY_COUNT; i++) {
    if (name == CITIES[i].name) return i;
  }
  return -1;
}

struct LocalWeatherTime {
  int month, day, hour, minute;
  bool valid;
};

// Open-Meteo's current_weather.time is ISO-8601 UTC with no offset suffix
// (e.g. "2026-08-14T15:00"). Shifts to the same UTC offset the Clock app uses
// (SETTINGS.clockUtcOffsetQ, in quarter-hours) and rolls the calendar fields
// by hand rather than mktime/timegm, which would pull in libc's unconfigured
// timezone state on this target (same approach as Football/F1's local-time
// conversion).
LocalWeatherTime toLocalWeatherTime(const std::string& iso) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) {
    return LocalWeatherTime{0, 0, 0, 0, false};
  }

  const int offsetMinutes = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  int totalMinutes = h * 60 + mi + offsetMinutes;

  auto daysInMonth = [](int year, int month) {
    static const int base[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return base[month - 1];
  };

  while (totalMinutes < 0) {
    totalMinutes += 24 * 60;
    d--;
    if (d < 1) {
      mo--;
      if (mo < 1) {
        mo = 12;
        y--;
      }
      d = daysInMonth(y, mo);
    }
  }
  while (totalMinutes >= 24 * 60) {
    totalMinutes -= 24 * 60;
    d++;
    if (d > daysInMonth(y, mo)) {
      d = 1;
      mo++;
      if (mo > 12) {
        mo = 1;
        y++;
      }
    }
  }

  return LocalWeatherTime{mo, d, totalMinutes / 60, totalMinutes % 60, true};
}

const char* getWeatherDesc(int code) {
  switch (code) {
    case 0:
      return tr(STR_WEATHER_CLEAR_SKY);
    case 1:
      return tr(STR_WEATHER_MAINLY_CLEAR);
    case 2:
      return tr(STR_WEATHER_PARTLY_CLOUDY);
    case 3:
      return tr(STR_WEATHER_OVERCAST);
    case 45:
    case 48:
      return tr(STR_WEATHER_FOGGY);
    case 51:
    case 53:
    case 55:
      return tr(STR_WEATHER_DRIZZLE);
    case 61:
    case 63:
    case 65:
      return tr(STR_WEATHER_RAINY);
    case 71:
    case 73:
    case 75:
      return tr(STR_WEATHER_SNOWY);
    case 80:
    case 81:
    case 82:
      return tr(STR_WEATHER_RAIN_SHOWERS);
    case 95:
    case 96:
    case 99:
      return tr(STR_WEATHER_THUNDERSTORM);
    default:
      return tr(STR_WEATHER_UNKNOWN);
  }
}

void saveConfig(int cityIndex, const std::string& cityName) {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/weather");
  JsonDocument doc;
  doc["city_index"] = cityIndex;
  doc["city_name"] = cityName;
  String output;
  serializeJson(doc, output);
  Storage.writeFile("/apps/weather/config.json", output);
}

bool loadConfig(int& cityIndex, std::string& cityName) {
  String input = Storage.readFile("/apps/weather/config.json");
  if (input.length() == 0) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, input);
  if (err) return false;
  cityIndex = doc["city_index"] | 0;
  cityName = doc["city_name"] | "";
  return true;
}

std::string getSafeCityFilename(const std::string& name) {
  std::string safe;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      safe += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else if (c == ' ') {
      safe += '_';
    }
  }
  return safe;
}

void saveCache(const std::string& cityName, double temp, double windspeed, int weathercode,
               const std::string& timeStr, bool isDay) {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/weather");
  JsonDocument doc;
  doc["temp"] = temp;
  doc["windspeed"] = windspeed;
  doc["weathercode"] = weathercode;
  doc["time"] = timeStr;
  doc["is_day"] = isDay;
  String output;
  serializeJson(doc, output);
  std::string filepath = "/apps/weather/" + getSafeCityFilename(cityName) + ".txt";
  Storage.writeFile(filepath.c_str(), output);
}

bool loadCache(const std::string& cityName, double& temp, double& windspeed, int& weathercode,
               std::string& timeStr, bool& isDay) {
  std::string filepath = "/apps/weather/" + getSafeCityFilename(cityName) + ".txt";
  String input = Storage.readFile(filepath.c_str());
  if (input.length() == 0) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, input);
  if (err) return false;
  temp = doc["temp"] | 0.0;
  windspeed = doc["windspeed"] | 0.0;
  weathercode = doc["weathercode"] | 0;
  timeStr = doc["time"] | "";
  isDay = doc["is_day"] | true;
  return true;
}

bool fetchWeather(double lat, double lon, double& temp, double& windspeed, int& weathercode, std::string& timeStr,
                  bool& isDay) {
  char url[256];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/"
           "forecast?latitude=%.4f&longitude=%.4f&current_weather=true",
           lat, lon);
  std::string response;
  if (!HttpDownloader::fetchUrl(url, response)) {
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) return false;

  if (doc["current_weather"].isNull()) return false;
  auto cw = doc["current_weather"];
  temp = cw["temperature"] | 0.0;
  windspeed = cw["windspeed"] | 0.0;
  weathercode = cw["weathercode"] | 0;
  timeStr = cw["time"] | "";
  isDay = (cw["is_day"] | 1) != 0;
  return true;
}

void drawWeatherIcon(const GfxRenderer& r, int cx, int cy, int code, bool isDay) {
  switch (code) {
    case 0:  // Clear sky
    case 1:  // Mainly clear
      if (isDay) {
        r.fillRoundedRect(cx - 20, cy - 20, 40, 40, 20, Color::Black);
        r.fillRoundedRect(cx - 15, cy - 15, 30, 30, 15, Color::White);
        r.fillRoundedRect(cx - 10, cy - 10, 20, 20, 10, Color::Black);

        r.drawLine(cx, cy - 30, cx, cy - 24, 2, true);
        r.drawLine(cx, cy + 24, cx, cy + 30, 2, true);
        r.drawLine(cx - 30, cy, cx - 24, cy, 2, true);
        r.drawLine(cx + 24, cy, cx + 30, cy, 2, true);
        r.drawLine(cx - 21, cy - 21, cx - 17, cy - 17, 2, true);
        r.drawLine(cx + 17, cy - 17, cx + 21, cy - 21, 2, true);
        r.drawLine(cx - 21, cy + 21, cx - 17, cy + 17, 2, true);
        r.drawLine(cx + 17, cy + 17, cx + 21, cy + 21, 2, true);
      } else {
        // Crescent: a full disc with a second, offset disc erased on top of
        // it -- same two-circle carve used for the small partly-cloudy moon
        // below, just at full size and with no rays.
        r.fillRoundedRect(cx - 20, cy - 20, 40, 40, 20, Color::Black);
        r.fillRoundedRect(cx - 8, cy - 32, 40, 40, 20, Color::White);
      }
      break;

    case 2:  // Partly cloudy
      if (isDay) {
        r.fillRoundedRect(cx + 12 - 15, cy - 12 - 15, 30, 30, 15, Color::Black);
        r.fillRoundedRect(cx + 12 - 11, cy - 12 - 11, 22, 22, 11, Color::White);
        r.fillRoundedRect(cx + 12 - 7, cy - 12 - 7, 14, 14, 7, Color::Black);

        r.drawLine(cx + 12, cy - 31, cx + 12, cy - 27, 2, true);
        r.drawLine(cx + 31, cy - 12, cx + 35, cy - 12, 2, true);
        r.drawLine(cx + 25, cy - 25, cx + 28, cy - 28, 2, true);
      } else {
        r.fillRoundedRect(cx + 12 - 15, cy - 12 - 15, 30, 30, 15, Color::Black);
        r.fillRoundedRect(cx + 12 - 6, cy - 12 - 24, 30, 30, 15, Color::White);
      }

      r.fillRoundedRect(cx - 32, cy - 2, 64, 32, 16, Color::White);

      r.fillRect(cx - 25, cy + 5, 50, 15, true);
      r.fillRoundedRect(cx - 25, cy - 8, 24, 24, 12, Color::Black);
      r.fillRoundedRect(cx - 12, cy - 16, 30, 30, 15, Color::Black);
      r.fillRoundedRect(cx + 10, cy - 4, 18, 18, 9, Color::Black);
      break;

    case 3:   // Overcast
    case 45:  // Foggy
    case 48:
      r.fillRect(cx - 30, cy, 60, 20, true);
      r.fillRoundedRect(cx - 30, cy - 12, 30, 30, 15, Color::Black);
      r.fillRoundedRect(cx - 15, cy - 22, 38, 38, 19, Color::Black);
      r.fillRoundedRect(cx + 10, cy - 6, 24, 24, 12, Color::Black);

      if (code == 45 || code == 48) {
        r.drawLine(cx - 35, cy + 24, cx + 35, cy + 24, 2, true);
        r.drawLine(cx - 25, cy + 30, cx + 25, cy + 30, 2, true);
      }
      break;

    case 51:  // Drizzle
    case 53:
    case 55:
    case 61:  // Rainy
    case 63:
    case 65:
    case 80:  // Rain showers
    case 81:
    case 82:
      r.fillRect(cx - 28, cy - 5, 56, 20, true);
      r.fillRoundedRect(cx - 28, cy - 15, 28, 28, 14, Color::Black);
      r.fillRoundedRect(cx - 14, cy - 24, 36, 36, 18, Color::Black);
      r.fillRoundedRect(cx + 10, cy - 9, 22, 22, 11, Color::Black);

      r.drawLine(cx - 18, cy + 18, cx - 22, cy + 26, 2, true);
      r.drawLine(cx - 6, cy + 18, cx - 10, cy + 26, 2, true);
      r.drawLine(cx + 8, cy + 18, cx + 4, cy + 26, 2, true);
      r.drawLine(cx + 20, cy + 18, cx + 16, cy + 26, 2, true);
      break;

    case 71:  // Snowy
    case 73:
    case 75:
      r.fillRect(cx - 28, cy - 5, 56, 20, true);
      r.fillRoundedRect(cx - 28, cy - 15, 28, 28, 14, Color::Black);
      r.fillRoundedRect(cx - 14, cy - 24, 36, 36, 18, Color::Black);
      r.fillRoundedRect(cx + 10, cy - 9, 22, 22, 11, Color::Black);

      r.drawLine(cx - 17, cy + 21, cx - 11, cy + 21, 2, true);
      r.drawLine(cx - 14, cy + 18, cx - 14, cy + 24, 2, true);

      r.drawLine(cx - 3, cy + 24, cx + 3, cy + 24, 2, true);
      r.drawLine(cx, cy + 21, cx, cy + 27, 2, true);

      r.drawLine(cx + 11, cy + 21, cx + 17, cy + 21, 2, true);
      r.drawLine(cx + 14, cy + 18, cx + 14, cy + 24, 2, true);
      break;

    case 95:  // Thunderstorm
    case 96:
    case 99:
      r.fillRect(cx - 28, cy - 5, 56, 20, true);
      r.fillRoundedRect(cx - 28, cy - 15, 28, 28, 14, Color::Black);
      r.fillRoundedRect(cx - 14, cy - 24, 36, 36, 18, Color::Black);
      r.fillRoundedRect(cx + 10, cy - 9, 22, 22, 11, Color::Black);

      r.drawLine(cx - 3, cy + 16, cx + 5, cy + 23, 2, true);
      r.drawLine(cx + 5, cy + 23, cx - 5, cy + 23, 2, true);
      r.drawLine(cx - 5, cy + 23, cx + 1, cy + 31, 2, true);
      break;

    default:
      r.drawRoundedRect(cx - 20, cy - 20, 40, 40, 2, 20, true);
      r.drawCenteredText(NOTOSANS_18_FONT_ID, cy - 9, "?", true, EpdFontFamily::BOLD);
      break;
  }
}
}  // namespace

void WeatherActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/weather");

  errorMessage.clear();

  int savedIndex = 0;
  std::string savedName;
  const int resolvedIndex = loadConfig(savedIndex, savedName) ? findCityIndex(savedName) : -1;

  if (resolvedIndex >= 0) {
    selectedCityIndex = resolvedIndex;
    cityName = savedName;
    if (loadCache(cityName, temp, windspeed, weatherCode, timeStr, isDay)) {
      weatherLoaded = true;
      state = WeatherState::ShowWeather;
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        performFetch();
      }
    } else {
      weatherLoaded = false;
      errorMessage = tr(STR_WEATHER_NO_CACHE_FETCHING);
      state = WeatherState::Loading;
      requestUpdate();
      performFetch();
    }
  } else {
    state = WeatherState::SelectCity;
    selectedCityIndex = 0;
  }
  requestUpdate();
}

void WeatherActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void WeatherActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::TOOLS_MENU);
    return;
  }

  if (state == WeatherState::SelectCity) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedCityIndex = (selectedCityIndex - 1 + CITY_COUNT) % CITY_COUNT;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedCityIndex = (selectedCityIndex + 1) % CITY_COUNT;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      cityName = CITIES[selectedCityIndex].name;
      saveConfig(selectedCityIndex, cityName);
      if (loadCache(cityName, temp, windspeed, weatherCode, timeStr, isDay)) {
        weatherLoaded = true;
        state = WeatherState::ShowWeather;
        requestUpdate();
        if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
          performFetch();
        }
      } else {
        weatherLoaded = false;
        errorMessage = tr(STR_WEATHER_NO_CACHE_FETCHING);
        state = WeatherState::Loading;
        requestUpdate();
        performFetch();
      }
    }
  } else if (state == WeatherState::ShowWeather) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = WeatherState::Loading;
      requestUpdate();
      performFetch();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      state = WeatherState::SelectCity;
      requestUpdate();
    }
  }
}

// Ensures WiFi is connected (prompting if necessary), then runs the blocking
// fetch. Replaces the source app's ensureWifiConnected()/background-task
// pair — there is no async handoff here, doWeatherFetch() always finishes
// before this call returns to the event loop.
void WeatherActivity::performFetch() {
  if (WiFi.status() == WL_CONNECTED) {
    doWeatherFetch();
    return;
  }

  WiFi.mode(WIFI_STA);
  startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                          [this](const ActivityResult& result) {
                            if (result.isCancelled) {
                              if (!weatherLoaded) errorMessage = tr(STR_WEATHER_NO_NETWORK_NO_CACHE);
                              state = WeatherState::ShowWeather;
                              requestUpdate();
                            } else {
                              doWeatherFetch();
                            }
                          });
}

void WeatherActivity::doWeatherFetch() {
  requestUpdateAndWait();  // paint the "Loading..." state before the blocking calls below

  // Stabilization delay for DNS resolution, matching the source app's timing.
  delay(500);

  double fetchedTemp = 0.0, fetchedWindspeed = 0.0;
  int fetchedWeatherCode = 0;
  std::string fetchedTimeStr;
  bool fetchedIsDay = true;
  bool success = false;
  int retries = 3;
  while (retries > 0) {
    if (fetchWeather(CITIES[selectedCityIndex].lat, CITIES[selectedCityIndex].lon, fetchedTemp, fetchedWindspeed,
                      fetchedWeatherCode, fetchedTimeStr, fetchedIsDay)) {
      success = true;
      break;
    }
    retries--;
    if (retries > 0) delay(1000);
  }

  if (success) {
    temp = fetchedTemp;
    windspeed = fetchedWindspeed;
    weatherCode = fetchedWeatherCode;
    timeStr = fetchedTimeStr;
    isDay = fetchedIsDay;
    weatherLoaded = true;
    saveCache(cityName, temp, windspeed, weatherCode, timeStr, isDay);
  } else if (!weatherLoaded) {
    errorMessage = tr(STR_WEATHER_NO_NETWORK_NO_CACHE);
  }
  state = WeatherState::ShowWeather;
  requestUpdate();
}

void WeatherActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == WeatherState::SelectCity) {
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   tr(STR_WEATHER_SELECT_CITY));

    GUI.drawButtonMenu(
        renderer,
        Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
             pageHeight - (metrics.headerHeight + metrics.topPadding + metrics.verticalSpacing + metrics.buttonHintsHeight)},
        CITY_COUNT, selectedCityIndex, [](int index) { return std::string(CITIES[index].name); },
        [](int) { return UIIcon::WeatherCity; });

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == WeatherState::Loading) {
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEATHER_TITLE));

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int contentHeight = contentBottom - contentTop;

    int textY = contentTop + contentHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_WEATHER_LOADING));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == WeatherState::ShowWeather) {
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEATHER_TITLE));

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int contentHeight = contentBottom - contentTop;

    const int cardX = metrics.contentSidePadding;
    const int cardW = pageWidth - 2 * metrics.contentSidePadding;
    const int cardH = 420;
    const int cardY = contentTop + (contentHeight - cardH) / 2;

    renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 2, 12, true);

    if (weatherLoaded) {
      drawWeatherIcon(renderer, cardX + cardW / 2, cardY + 70, weatherCode, isDay);

      char tempBuf[64];
      double tempF = temp * 9.0 / 5.0 + 32.0;
      snprintf(tempBuf, sizeof(tempBuf), "%.1f °C / %.1f °F", temp, tempF);
      renderer.drawCenteredText(NOTOSANS_18_FONT_ID, cardY + 145, tempBuf, true, EpdFontFamily::BOLD);

      renderer.drawCenteredText(NOTOSANS_16_FONT_ID, cardY + 195, cityName.c_str(), true, EpdFontFamily::BOLD);

      const char* desc = getWeatherDesc(weatherCode);
      renderer.drawCenteredText(NOTOSANS_14_FONT_ID, cardY + 235, desc, true, EpdFontFamily::BOLD);

      char windBuf[64];
      double windspeedMph = windspeed * 0.621371;
      snprintf(windBuf, sizeof(windBuf), tr(STR_WEATHER_WIND_FORMAT), windspeed, windspeedMph);
      renderer.drawCenteredText(NOTOSANS_12_FONT_ID, cardY + 275, windBuf, true, EpdFontFamily::REGULAR);

      char timeBuf[64];
      const LocalWeatherTime lt = toLocalWeatherTime(timeStr);
      if (lt.valid) {
        char localBuf[16];
        snprintf(localBuf, sizeof(localBuf), "%02d/%02d %02d:%02d", lt.day, lt.month, lt.hour, lt.minute);
        snprintf(timeBuf, sizeof(timeBuf), tr(STR_WEATHER_UPDATED_FORMAT), localBuf);
      } else {
        snprintf(timeBuf, sizeof(timeBuf), tr(STR_WEATHER_UPDATED_FORMAT), timeStr.c_str());
      }
      renderer.drawCenteredText(SMALL_FONT_ID, cardY + 315, timeBuf, true, EpdFontFamily::REGULAR);

    } else {
      int textY = cardY + cardH / 2 - renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, textY, errorMessage.c_str(), true, EpdFontFamily::BOLD);
    }

    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_WEATHER_REFRESH), tr(STR_WEATHER_CITY), nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

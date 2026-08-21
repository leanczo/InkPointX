#include "StravaActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "StravaTokenStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"  // NetworkFileTransaction
#include "network/StravaApiClient.h"
#include "util/HoldGestures.h"

// Strava app credentials are personal to whoever builds this firmware, so
// they're never hardcoded here - set them via platformio.local.ini
// (gitignored, see .skills/SKILL.md), e.g.:
//   build_flags =
//     ${base.build_flags}
//     -DSTRAVA_CLIENT_ID=\"your_client_id\"
//     -DSTRAVA_CLIENT_SECRET=\"your_client_secret\"
//     -DSTRAVA_INITIAL_REFRESH_TOKEN=\"your_current_refresh_token\"
// (the refresh token seed lives in StravaTokenStore.cpp, next to the store
// that owns its rotated replacement.)
#ifndef STRAVA_CLIENT_ID
#define STRAVA_CLIENT_ID ""
#endif
#ifndef STRAVA_CLIENT_SECRET
#define STRAVA_CLIENT_SECRET ""
#endif

namespace {

constexpr const char* kAthletePath = "/apps/strava/athlete.json";
constexpr const char* kAthleteRawPath = "/apps/strava/athlete_raw.json";

// LOG_ERR compiles to nothing unless ENABLE_SERIAL_LOG is set, and even then
// needs a serial monitor attached to read it - a bounded, stack-only line
// appended straight to SD survives both and needs no USB to inspect. Same
// fix PersonalTrackerActivity::doFetch applies for the same reason; see also
// StravaApiClient.cpp's own copy for the lower-level HTTP/TLS failure detail.
void writeDebugLine(const char* line) {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/strava");
  HalFile debugFile = Storage.open("/apps/strava/debug.log", O_WRITE | O_CREAT | O_APPEND);
  if (!debugFile) return;
  debugFile.write(line, strlen(line));
  debugFile.close();
}

// Writes `content` to `path` transactionally (hidden temp file, atomic
// rename only on full success) so a failed refresh never corrupts the
// previous good cache - same guarantee HttpDownloader::downloadToFile gives
// every other cached-JSON activity in this codebase, mirrored here because
// this client fetches into a std::string rather than straight to a file.
bool writeStravaCache(const char* path, const std::string& content) {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/strava");

  const String finalPath = path;
  String tempPath;
  if (!NetworkFileTransaction::prepare(finalPath, ".strava-tmp", "STRAVA", tempPath)) {
    LOG_ERR("STRAVA", "Failed to prepare cache write: %s", path);
    return false;
  }
  const String data = content.c_str();
  if (!Storage.writeFile(tempPath.c_str(), data)) {
    Storage.remove(tempPath.c_str());
    LOG_ERR("STRAVA", "Failed to write staged cache: %s", path);
    return false;
  }
  if (!NetworkFileTransaction::commit(finalPath, tempPath, "STRAVA")) {
    Storage.remove(tempPath.c_str());
    LOG_ERR("STRAVA", "Failed to commit cache: %s", path);
    return false;
  }
  return true;
}

std::string formatDistanceKm(double meters) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.1f km", meters / 1000.0);
  return buf;
}

std::string formatDuration(int totalSeconds) {
  if (totalSeconds <= 0) return "0m";
  const int hours = totalSeconds / 3600;
  const int minutes = (totalSeconds % 3600) / 60;
  char buf[24];
  if (hours > 0) {
    snprintf(buf, sizeof(buf), "%dh %dm", hours, minutes);
  } else {
    snprintf(buf, sizeof(buf), "%dm", minutes);
  }
  return buf;
}

std::string formatElevation(double meters) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.0f m", meters);
  return buf;
}

// Minutes:seconds per km, for foot-based sports where pace is the natural
// unit (as opposed to km/h for wheeled/water sports - see isPaceSport).
std::string formatPace(double metersPerSecond) {
  if (metersPerSecond <= 0.01) return "-";
  const double secPerKm = 1000.0 / metersPerSecond;
  const int minutes = static_cast<int>(secPerKm / 60);
  const int seconds = static_cast<int>(secPerKm) % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d/km", minutes, seconds);
  return buf;
}

std::string formatSpeed(double metersPerSecond) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f km/h", metersPerSecond * 3.6);
  return buf;
}

// The API returns local start times as "YYYY-MM-DDTHH:MM:SS" - every row is
// recent enough that the year is noise, same reasoning as FormulaOne's
// shortDate for race dates.
std::string shortDate(const std::string& isoDate) {
  if (isoDate.size() < 10) return isoDate;
  return isoDate.substr(8, 2) + "/" + isoDate.substr(5, 2);
}

bool isPaceSport(const std::string& sportType) {
  return sportType == "Run" || sportType == "TrailRun" || sportType == "Walk" || sportType == "Hike" ||
        sportType == "VirtualRun";
}

// Curated labels for the sport types this dashboard's Stats tab covers, plus
// the few others common enough to be worth translating. Anything else falls
// back to Strava's own sport_type string verbatim (foreign API data, not app
// UI text - same treatment FormulaOneActivity gives driver/team names).
std::string sportTypeLabel(const std::string& sportType) {
  if (sportType == "Run") return tr(STR_STRAVA_SPORT_RUN);
  if (sportType == "Ride") return tr(STR_STRAVA_SPORT_RIDE);
  if (sportType == "Swim") return tr(STR_STRAVA_SPORT_SWIM);
  return sportType;
}

const char* sportSelectorLabel(int sportIndex) {
  switch (sportIndex) {
    case 0:
      return tr(STR_STRAVA_SPORT_RUN);
    case 1:
      return tr(STR_STRAVA_SPORT_RIDE);
    default:
      return tr(STR_STRAVA_SPORT_SWIM);
  }
}

// A single cell of the stat grid (see render()) - bold value over a small
// label, both centered. Same layout idiom as FormulaOneActivity's
// drawDriverStatCell / BookStatsView's drawStatCell.
void drawStravaStatCell(const GfxRenderer& renderer, int x, int y, int w, int h, const std::string& value,
                        const char* label) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int maxTextW = std::max(0, w - 8);
  const auto fittedValue = renderer.truncatedText(UI_12_FONT_ID, value.c_str(), maxTextW, EpdFontFamily::BOLD);
  const auto fittedLabel = renderer.truncatedText(SMALL_FONT_ID, label, maxTextW);
  const int totalTextH = valueLineH + 4 + labelLineH;
  const int textY = y + std::max(0, (h - totalTextH) / 2);
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, fittedValue.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + (w - valueW) / 2, textY, fittedValue.c_str(), true, EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(SMALL_FONT_ID, fittedLabel.c_str());
  renderer.drawText(SMALL_FONT_ID, x + (w - labelW) / 2, textY + valueLineH + 4, fittedLabel.c_str(), true);
}

// Three horizontal bars comparing the selected sport's distance across the
// last 4 weeks / year-to-date / all-time. No shared chart component exists
// in this codebase (BookStatsView's drawHorizontalBars is a compile-time
// template keyed on StrId labels, which doesn't fit runtime-formatted
// distance strings) - written directly here, same manual
// fillRect/drawText approach BookStatsView's version uses internally.
void drawStravaDistanceBars(const GfxRenderer& renderer, int x, int y, int w, int h, const double distances[3],
                            const char* labels[3]) {
  constexpr int kRows = 3;
  constexpr int labelColW = 90;
  constexpr int barLeftGap = 8;
  constexpr int rightPadding = 12;
  const int rowH = h / kRows;
  const int barH = std::max(8, rowH - 12);
  double maxDist = 0;
  for (int i = 0; i < kRows; i++) maxDist = std::max(maxDist, distances[i]);

  const int barX = x + labelColW + barLeftGap;
  const int barMaxW = std::max(0, w - labelColW - barLeftGap - rightPadding);
  for (int i = 0; i < kRows; i++) {
    const int rowTop = y + i * rowH;
    const int barY = rowTop + (rowH - barH) / 2;
    const int labelY = rowTop + (rowH - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, x, labelY, labels[i]);
    int fillW = 0;
    if (maxDist > 0 && distances[i] > 0) {
      fillW = std::max(2, static_cast<int>((barMaxW * distances[i]) / maxDist));
    }
    if (fillW > 0) renderer.fillRect(barX, barY, fillW, barH, true);
    const std::string valueText = formatDistanceKm(distances[i]);
    renderer.drawText(SMALL_FONT_ID, barX + fillW + barLeftGap, labelY, valueText.c_str());
  }
}

// drawCenteredText is single-line only; a long error message (e.g. the wifi-
// required or generic fetch-failed text) would otherwise get truncated at
// the screen edge instead of wrapping - same fix PersonalTrackerActivity and
// FormulaOneActivity apply for their own loading/error states.
void drawWrappedMessage(const GfxRenderer& renderer, int pageWidth, int contentSidePadding, int top, int bottom,
                        const char* msg) {
  const int width = pageWidth - 2 * contentSidePadding;
  auto lines = renderer.wrappedText(UI_12_FONT_ID, msg, width, 2, EpdFontFamily::REGULAR);
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  int y = top + (bottom - top) / 2 - (static_cast<int>(lines.size()) * lineH) / 2;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str());
    y += lineH;
  }
}

}  // namespace

std::string StravaActivity::apiUrl(int tab) const {
  if (tab == static_cast<int>(StravaTab::Stats)) {
    return "https://www.strava.com/api/v3/athletes/" + athleteId + "/stats";
  }
  return "https://www.strava.com/api/v3/athlete/activities?per_page=30";
}

std::string StravaActivity::cachePath(int tab) const {
  return tab == static_cast<int>(StravaTab::Stats) ? "/apps/strava/stats.json" : "/apps/strava/activities.json";
}

bool StravaActivity::ensureAccessToken() {
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  // 60s safety margin so a request in flight doesn't cross the expiry.
  if (!STRAVA_TOKEN_STORE.accessToken().empty() && STRAVA_TOKEN_STORE.expiresAtEpoch() > now + 60) {
    return true;
  }
  if (!STRAVA_TOKEN_STORE.hasRefreshToken()) {
    LOG_ERR("STRAVA", "No Strava refresh token configured - see platformio.local.ini");
    writeDebugLine("[strava] ensureAccessToken: no refresh token configured (check platformio.local.ini)\n");
    return false;
  }

  StravaApiClient::TokenRefreshResult result;
  if (!StravaApiClient::refreshAccessToken(STRAVA_CLIENT_ID, STRAVA_CLIENT_SECRET, STRAVA_TOKEN_STORE.refreshToken(),
                                           result)) {
    return false;
  }
  // Persist the rotated refresh_token immediately, before using the new
  // access token for anything else - a crash between refresh and first use
  // must never strand the device on an already-rotated (now-invalid) token.
  STRAVA_TOKEN_STORE.save(result.refreshToken, result.accessToken, result.expiresAtEpoch);
  return true;
}

bool StravaActivity::loadAthleteFromSd() {
  const String input = Storage.readFile(kAthletePath);
  if (input.length() == 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, input.c_str())) return false;
  athleteId = doc["id"] | std::string("");
  athleteFirstName = doc["firstname"] | std::string("");
  if (athleteId.empty()) return false;
  athleteLoaded = true;
  return true;
}

bool StravaActivity::ensureAthlete() {
  if (athleteLoaded && !athleteId.empty()) return true;
  if (loadAthleteFromSd()) return true;
  if (!ensureAccessToken()) return false;

  if (!StravaApiClient::authenticatedGetToFile("https://www.strava.com/api/v3/athlete",
                                               STRAVA_TOKEN_STORE.accessToken(), kAthleteRawPath)) {
    return false;
  }

  const String input = Storage.readFile(kAthleteRawPath);
  Storage.remove(kAthleteRawPath);
  if (input.length() == 0) {
    writeDebugLine("[strava] athlete fetch: staged file was empty\n");
    return false;
  }

  JsonDocument filter;
  filter["id"] = true;
  filter["firstname"] = true;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, input.c_str(), DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("STRAVA", "Athlete JSON parse failed: %s", err.c_str());
    char buf[128];
    snprintf(buf, sizeof(buf), "[strava] athlete JSON parse failed: %s\n", err.c_str());
    writeDebugLine(buf);
    return false;
  }

  const uint64_t id = doc["id"] | 0ULL;
  if (id == 0) {
    writeDebugLine("[strava] athlete fetch: response had no \"id\" field\n");
    return false;
  }
  athleteId = std::to_string(id);
  athleteFirstName = doc["firstname"] | std::string("");
  athleteLoaded = true;

  JsonDocument out;
  out["id"] = athleteId;
  out["firstname"] = athleteFirstName;
  String json;
  serializeJson(out, json);
  writeStravaCache(kAthletePath, std::string(json.c_str()));

  return true;
}

void StravaActivity::startFetch(int tab) {
  refreshing[tab] = true;
  refreshFailed[tab] = false;
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this, tab](const ActivityResult& result) {
                              if (result.isCancelled) {
                                refreshing[tab] = false;
                                if (!loaded[tab]) {
                                  errorMessage[tab] = tr(STR_STRAVA_WIFI_REQUIRED);
                                } else {
                                  refreshFailed[tab] = true;
                                }
                                requestUpdate();
                              } else {
                                doFetch(tab);
                              }
                            });
    return;
  }

  doFetch(tab);
}

void StravaActivity::doFetch(int tab) {
  requestUpdateAndWait();  // paint the "Refreshing..." state before the blocking calls below
  wifiWasUsed = true;

  bool ok = ensureAccessToken();
  if (!ok) writeDebugLine("[strava] doFetch: ensureAccessToken failed\n");

  if (ok && tab == static_cast<int>(StravaTab::Stats)) {
    ok = ensureAthlete();
    if (!ok) writeDebugLine("[strava] doFetch: ensureAthlete failed\n");
  }

  // Streams straight to cachePath(tab) - see StravaApiClient::authenticatedGetToFile
  // for why this doesn't buffer the raw response in RAM first.
  if (ok) {
    ok = StravaApiClient::authenticatedGetToFile(apiUrl(tab), STRAVA_TOKEN_STORE.accessToken(), cachePath(tab));
    if (!ok) {
      char buf[64];
      snprintf(buf, sizeof(buf), "[strava] doFetch: GET failed for tab=%d\n", tab);
      writeDebugLine(buf);
    }
  }

  refreshing[tab] = false;

  if (ok) {
    loadCacheFromSd(tab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      errorMessage[tab] = tr(STR_STRAVA_NO_DATA);
      writeDebugLine("[strava] doFetch: downloaded OK but cache failed to parse\n");
    }
  } else if (!loaded[tab]) {
    errorMessage[tab] = tr(STR_STRAVA_NO_DATA);
  } else {
    // Refresh failed but we still have good cached data from before - keep
    // showing it instead of an error screen, just flag it so render() can
    // surface a brief "couldn't update" banner instead of failing silently.
    refreshFailed[tab] = true;
  }
  requestUpdate();
}

bool StravaActivity::loadCacheFromSd(int tab) {
  const String input = Storage.readFile(cachePath(tab).c_str());
  if (input.length() == 0) return false;
  const std::string json(input.c_str());
  const bool ok =
      (tab == static_cast<int>(StravaTab::Stats)) ? parseStats(json) : parseActivities(json);
  loaded[tab] = ok;
  if (ok) errorMessage[tab].clear();
  return ok;
}

bool StravaActivity::parseStats(const std::string& json) {
  // Only keep the fields this screen renders - cuts ArduinoJson's parsed-tree
  // memory well below what the full response (biggest_ride_distance,
  // biggest_climb_elevation_gain, achievement_count, etc.) would need.
  JsonDocument filter;
  for (const char* key : {"recent_run_totals", "recent_ride_totals", "recent_swim_totals", "ytd_run_totals",
                          "ytd_ride_totals", "ytd_swim_totals", "all_run_totals", "all_ride_totals",
                          "all_swim_totals"}) {
    filter[key]["count"] = true;
    filter[key]["distance"] = true;
    filter[key]["moving_time"] = true;
    filter[key]["elevation_gain"] = true;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("STRAVA", "Stats JSON parse failed (%u bytes): %s", static_cast<unsigned>(json.size()), err.c_str());
    char buf[128];
    snprintf(buf, sizeof(buf), "[strava] stats parse failed (%u bytes): %s\n", static_cast<unsigned>(json.size()),
            err.c_str());
    writeDebugLine(buf);
    return false;
  }

  auto readTotals = [&doc](const char* key) {
    JsonObject obj = doc[key];
    StravaSportTotals t;
    t.count = obj["count"] | 0;
    t.distanceMeters = obj["distance"] | 0.0;
    t.movingTimeSec = obj["moving_time"] | 0;
    t.elevationGainMeters = obj["elevation_gain"] | 0.0;
    return t;
  };

  StravaStats newStats;
  newStats.recent[0] = readTotals("recent_run_totals");
  newStats.recent[1] = readTotals("recent_ride_totals");
  newStats.recent[2] = readTotals("recent_swim_totals");
  newStats.ytd[0] = readTotals("ytd_run_totals");
  newStats.ytd[1] = readTotals("ytd_ride_totals");
  newStats.ytd[2] = readTotals("ytd_swim_totals");
  newStats.allTime[0] = readTotals("all_run_totals");
  newStats.allTime[1] = readTotals("all_ride_totals");
  newStats.allTime[2] = readTotals("all_swim_totals");

  stats = newStats;
  return true;
}

bool StravaActivity::parseActivities(const std::string& json) {
  JsonDocument filter;
  filter[0]["name"] = true;
  filter[0]["sport_type"] = true;
  filter[0]["distance"] = true;
  filter[0]["moving_time"] = true;
  filter[0]["total_elevation_gain"] = true;
  filter[0]["start_date_local"] = true;
  filter[0]["average_speed"] = true;
  filter[0]["kudos_count"] = true;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("STRAVA", "Activities JSON parse failed (%u bytes): %s", static_cast<unsigned>(json.size()), err.c_str());
    char buf[128];
    snprintf(buf, sizeof(buf), "[strava] activities parse failed (%u bytes): %s\n", static_cast<unsigned>(json.size()),
            err.c_str());
    writeDebugLine(buf);
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  std::vector<StravaActivityItem> newItems;
  newItems.reserve(arr.size());
  for (JsonObject item : arr) {
    StravaActivityItem entry;
    entry.name = item["name"] | std::string("");
    entry.sportType = item["sport_type"] | std::string("");
    entry.distanceMeters = item["distance"] | 0.0;
    entry.movingTimeSec = item["moving_time"] | 0;
    entry.elevationGainMeters = item["total_elevation_gain"] | 0.0;
    entry.startDateLocal = item["start_date_local"] | std::string("");
    entry.averageSpeed = item["average_speed"] | 0.0;
    entry.kudosCount = item["kudos_count"] | 0;
    if (entry.name.empty()) continue;
    newItems.push_back(std::move(entry));
  }

  // An empty list is a legitimate "no recent activities" state, not a parse
  // failure - render() shows drawEmptyState for it instead of an error.
  recentActivities = std::move(newItems);
  return true;
}

void StravaActivity::openActivityDetail(int index) {
  if (index < 0 || index >= static_cast<int>(recentActivities.size())) return;
  detailActivityIndex = index;
  showingActivityDetail = true;
  requestUpdate();
}

void StravaActivity::onEnter() {
  Activity::onEnter();
  STRAVA_TOKEN_STORE.load();
  char buf[128];
  snprintf(buf, sizeof(buf), "[strava] onEnter: hasRefreshToken=%d clientIdLen=%u clientSecretLen=%u\n",
          STRAVA_TOKEN_STORE.hasRefreshToken(), static_cast<unsigned>(strlen(STRAVA_CLIENT_ID)),
          static_cast<unsigned>(strlen(STRAVA_CLIENT_SECRET)));
  writeDebugLine(buf);
  loadAthleteFromSd();

  for (int tab = 0; tab < STRAVA_TAB_COUNT; tab++) {
    if (!loaded[tab]) loadCacheFromSd(tab);
  }
  if (!loaded[static_cast<int>(currentTab)]) {
    startFetch(static_cast<int>(currentTab));
  }
  requestUpdate();
}

void StravaActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void StravaActivity::loop() {
  if (showingActivityDetail) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      showingActivityDetail = false;
      requestUpdate();
    }
    return;
  }

  // Activities tab: tap-Confirm opens the selected row's detail, so refresh
  // moves to a hold there - same idiom as FormulaOneActivity's Drivers tab
  // (driverRefreshHoldFired). The Stats tab has no competing tap action
  // (Up/Down already cycles the selected sport), so its Confirm just
  // refreshes directly, like F1's Constructors/Calendar tabs.
  if (currentTab == StravaTab::Activities) {
    if (refreshHoldFired) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
        refreshHoldFired = false;
      }
      return;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= HoldGestures::SHORT_MS) {
      refreshHoldFired = true;
      startFetch(static_cast<int>(currentTab));
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::APPS_MENU);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    currentTab = (currentTab == StravaTab::Stats) ? StravaTab::Activities : StravaTab::Stats;
    const int tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty()) startFetch(tab);
    requestUpdate();
  } else if (currentTab == StravaTab::Stats && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    selectedSport = (selectedSport + 2) % 3;
    requestUpdate();
  } else if (currentTab == StravaTab::Stats && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selectedSport = (selectedSport + 1) % 3;
    requestUpdate();
  } else if (currentTab == StravaTab::Stats && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startFetch(static_cast<int>(currentTab));
  } else if (currentTab == StravaTab::Activities && !recentActivities.empty() &&
            mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    selectedRow[1] = static_cast<int>((selectedRow[1] - 1 + recentActivities.size()) % recentActivities.size());
    requestUpdate();
  } else if (currentTab == StravaTab::Activities && !recentActivities.empty() &&
            mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selectedRow[1] = static_cast<int>((selectedRow[1] + 1) % recentActivities.size());
    requestUpdate();
  } else if (currentTab == StravaTab::Activities && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openActivityDetail(selectedRow[1]);
  }
}

void StravaActivity::drawTabStrip(int y, const std::vector<std::string>& labels, int selectedIndex) {
  const auto pageWidth = renderer.getScreenWidth();
  const int count = static_cast<int>(labels.size());
  const int tabW = (pageWidth - 40) / count;
  constexpr int kTabH = 30;
  const int tabTextY = y + (kTabH - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
  for (int i = 0; i < count; i++) {
    const bool active = (i == selectedIndex);
    const int tx = 20 + i * tabW;
    renderer.drawRoundedRect(tx + 2, y, tabW - 4, kTabH, 1, 5, true);
    if (active) {
      renderer.fillRoundedRect(tx + 2, y, tabW - 4, kTabH, 5, Color::Black);
    }
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, labels[i].c_str());
    renderer.drawText(SMALL_FONT_ID, tx + (tabW - textW) / 2, tabTextY, labels[i].c_str(), !active);
  }
}

void StravaActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_STRAVA_TITLE));

  if (showingActivityDetail && detailActivityIndex >= 0 &&
      detailActivityIndex < static_cast<int>(recentActivities.size())) {
    const auto& item = recentActivities[detailActivityIndex];
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight}, item.name.c_str(),
                      shortDate(item.startDateLocal).c_str());

    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int cardX = metrics.contentSidePadding;
    const int cardW = pageWidth - 2 * metrics.contentSidePadding;
    const int cardY = contentTop + metrics.subHeaderHeight + metrics.verticalSpacing;
    const int cardH = listBottom - cardY;
    const int rowH = cardH / 3;
    const int halfW = cardW / 2;

    renderer.drawRect(cardX, cardY, cardW, cardH);
    renderer.drawLine(cardX, cardY + rowH, cardX + cardW, cardY + rowH);
    renderer.drawLine(cardX, cardY + rowH * 2, cardX + cardW, cardY + rowH * 2);
    renderer.drawLine(cardX + halfW, cardY, cardX + halfW, cardY + cardH);

    const bool pace = isPaceSport(item.sportType);
    drawStravaStatCell(renderer, cardX, cardY, halfW, rowH, formatDistanceKm(item.distanceMeters),
                       tr(STR_STRAVA_STAT_DISTANCE));
    drawStravaStatCell(renderer, cardX + halfW, cardY, halfW, rowH, formatDuration(item.movingTimeSec),
                       tr(STR_STRAVA_STAT_TIME));
    drawStravaStatCell(renderer, cardX, cardY + rowH, halfW, rowH,
                       pace ? formatPace(item.averageSpeed) : formatSpeed(item.averageSpeed),
                       pace ? tr(STR_STRAVA_STAT_PACE) : tr(STR_STRAVA_STAT_SPEED));
    drawStravaStatCell(renderer, cardX + halfW, cardY + rowH, halfW, rowH, formatElevation(item.elevationGainMeters),
                       tr(STR_STRAVA_STAT_ELEVATION));
    drawStravaStatCell(renderer, cardX, cardY + rowH * 2, halfW, rowH, std::to_string(item.kudosCount),
                       tr(STR_STRAVA_STAT_KUDOS));
    drawStravaStatCell(renderer, cardX + halfW, cardY + rowH * 2, halfW, rowH, sportTypeLabel(item.sportType),
                       tr(STR_STRAVA_STAT_SPORT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int tabBarY = metrics.topPadding + metrics.headerHeight + 20;
  const std::vector<std::string> tabLabels = {tr(STR_STRAVA_TAB_STATS), tr(STR_STRAVA_TAB_ACTIVITIES)};
  drawTabStrip(tabBarY, tabLabels, static_cast<int>(currentTab));
  int contentTop = tabBarY + 30 + metrics.verticalSpacing;

  const int tab = static_cast<int>(currentTab);
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (currentTab == StravaTab::Stats) {
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight},
                      athleteFirstName.empty() ? tr(STR_STRAVA_TITLE) : athleteFirstName.c_str(),
                      sportSelectorLabel(selectedSport));
    contentTop += metrics.subHeaderHeight + metrics.verticalSpacing;

    if (!loaded[tab] && !refreshing[tab]) {
      const char* msg = !errorMessage[tab].empty() ? errorMessage[tab].c_str() : tr(STR_STRAVA_LOADING);
      drawWrappedMessage(renderer, pageWidth, metrics.contentSidePadding, contentTop, listBottom, msg);
    } else if (loaded[tab]) {
      const int cardX = metrics.contentSidePadding;
      const int cardW = pageWidth - 2 * metrics.contentSidePadding;
      constexpr int chartH = 110;
      renderer.drawRect(cardX, contentTop, cardW, chartH);
      const double distances[3] = {stats.recent[selectedSport].distanceMeters, stats.ytd[selectedSport].distanceMeters,
                                   stats.allTime[selectedSport].distanceMeters};
      const char* barLabels[3] = {tr(STR_STRAVA_CHART_RECENT), tr(STR_STRAVA_CHART_YTD), tr(STR_STRAVA_CHART_ALL)};
      drawStravaDistanceBars(renderer, cardX + 10, contentTop + 6, cardW - 20, chartH - 12, distances, barLabels);

      const int gridY = contentTop + chartH + metrics.verticalSpacing;
      const int gridH = listBottom - gridY;
      const int rowH = gridH / 2;
      const int colW = cardW / 4;
      renderer.drawRect(cardX, gridY, cardW, gridH);
      renderer.drawLine(cardX, gridY + rowH, cardX + cardW, gridY + rowH);
      renderer.drawLine(cardX + colW, gridY, cardX + colW, gridY + gridH);
      renderer.drawLine(cardX + colW * 2, gridY, cardX + colW * 2, gridY + gridH);
      renderer.drawLine(cardX + colW * 3, gridY, cardX + colW * 3, gridY + gridH);

      const auto& ytd = stats.ytd[selectedSport];
      const auto& allTime = stats.allTime[selectedSport];
      drawStravaStatCell(renderer, cardX, gridY, colW, rowH, formatDistanceKm(ytd.distanceMeters),
                         tr(STR_STRAVA_STAT_DISTANCE));
      drawStravaStatCell(renderer, cardX + colW, gridY, colW, rowH, formatDuration(ytd.movingTimeSec),
                         tr(STR_STRAVA_STAT_TIME));
      drawStravaStatCell(renderer, cardX + colW * 2, gridY, colW, rowH, formatElevation(ytd.elevationGainMeters),
                         tr(STR_STRAVA_STAT_ELEVATION));
      drawStravaStatCell(renderer, cardX + colW * 3, gridY, cardW - colW * 3, rowH, std::to_string(ytd.count),
                         tr(STR_STRAVA_STAT_COUNT));

      drawStravaStatCell(renderer, cardX, gridY + rowH, colW, rowH, formatDistanceKm(allTime.distanceMeters),
                         tr(STR_STRAVA_STAT_DISTANCE));
      drawStravaStatCell(renderer, cardX + colW, gridY + rowH, colW, rowH, formatDuration(allTime.movingTimeSec),
                         tr(STR_STRAVA_STAT_TIME));
      drawStravaStatCell(renderer, cardX + colW * 2, gridY + rowH, colW, rowH,
                         formatElevation(allTime.elevationGainMeters), tr(STR_STRAVA_STAT_ELEVATION));
      drawStravaStatCell(renderer, cardX + colW * 3, gridY + rowH, cardW - colW * 3, rowH,
                         std::to_string(allTime.count), tr(STR_STRAVA_STAT_COUNT));
    }
  } else {
    if (!loaded[tab] && !refreshing[tab]) {
      const char* msg = !errorMessage[tab].empty() ? errorMessage[tab].c_str() : tr(STR_STRAVA_LOADING);
      drawWrappedMessage(renderer, pageWidth, metrics.contentSidePadding, contentTop, listBottom, msg);
    } else if (recentActivities.empty()) {
      GUI.drawEmptyState(renderer, Rect{0, contentTop, pageWidth, listBottom - contentTop}, tr(STR_STRAVA_EMPTY));
    } else {
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, listBottom - contentTop}, static_cast<int>(recentActivities.size()),
          selectedRow[1], [this](int i) { return recentActivities[i].name; },
          [this](int i) {
            return sportTypeLabel(recentActivities[i].sportType) + "  \xC2\xB7  " +
                  shortDate(recentActivities[i].startDateLocal);
          },
          nullptr, [this](int i) { return formatDistanceKm(recentActivities[i].distanceMeters); }, true);
    }
  }

  if (refreshing[tab]) {
    GUI.drawPopup(renderer, tr(STR_STRAVA_REFRESHING));
  } else if (refreshFailed[tab]) {
    GUI.drawPopup(renderer, tr(STR_STRAVA_REFRESH_FAILED));
  }

  const char* confirmHint = nullptr;
  if (currentTab == StravaTab::Activities) {
    confirmHint = recentActivities.empty() ? nullptr : tr(STR_STRAVA_ACTIVITY_DETAIL);
  } else {
    confirmHint = tr(STR_STRAVA_REFRESH);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

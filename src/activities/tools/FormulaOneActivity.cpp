#include "FormulaOneActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/HoldGestures.h"

namespace {

// Mirrors HttpDownloader.cpp's private MIN_TLS_LARGEST_BLOCK — a failed
// download with plenty of free heap but no block this large left is heap
// fragmentation, not a real network/server error (see doFetch()). Keep the
// two values in sync if that threshold ever changes.
constexpr size_t kMinTlsContiguousHeap = 32 * 1024;

// The API returns dates as ISO "YYYY-MM-DD". Every race in a season falls in
// the same year, so showing it on every row/subheader is just noise — this
// keeps only day/month, in the DD/MM order used in this app's locale.
std::string shortDate(const std::string& isoDate) {
  if (isoDate.size() < 10) return isoDate;  // unexpected shape: show as-is rather than mangle it
  return isoDate.substr(8, 2) + "/" + isoDate.substr(5, 2);
}

// Same DD/MM reordering as shortDate, but keeping the year — needed for a
// birth date, where the year is the point (unlike a same-season race date).
std::string fullDate(const std::string& isoDate) {
  if (isoDate.size() < 10) return isoDate;
  return isoDate.substr(8, 2) + "/" + isoDate.substr(5, 2) + "/" + isoDate.substr(0, 4);
}

// Age in whole years as of today (device clock, UTC) — same date-only
// comparison idiom as isSessionPast below, no timezone handling needed for a
// birth-year calculation.
std::string computeAge(const std::string& isoDateOfBirth) {
  if (isoDateOfBirth.size() < 10) return "";
  int by = 0, bm = 0, bd = 0;
  sscanf(isoDateOfBirth.c_str(), "%d-%d-%d", &by, &bm, &bd);
  const time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  int age = (t.tm_year + 1900) - by;
  if (t.tm_mon + 1 < bm || (t.tm_mon + 1 == bm && t.tm_mday < bd)) age--;
  return age >= 0 ? std::to_string(age) : "";
}

// A session needs date *and* time — a same-day comparison isn't enough to
// tell "FP1 already ran this morning" from "FP1 is later today". ISO
// date/time strings compare correctly as plain strings once concatenated in
// the same zero-padded shape, so no date parsing/arithmetic is needed here.
bool isSessionPast(const std::string& isoDate, const std::string& isoTime) {
  if (isoDate.size() < 10 || isoTime.size() < 8) return false;
  const time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  char nowBuf[20];
  snprintf(nowBuf, sizeof(nowBuf), "%04d-%02d-%02dT%02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
           t.tm_min, t.tm_sec);
  const std::string sessionDt = isoDate.substr(0, 10) + "T" + isoTime.substr(0, 8);
  return sessionDt <= std::string(nowBuf);
}

// Whether this weekend's *first* non-empty session (chronological order —
// same order showSessionSchedule's addSession calls use) has already
// happened. Checked fresh every render against calendarWeekends, not baked
// into rows[Calendar] at parse time, since "has this passed" changes purely
// with wall-clock time while the app may be sitting on another tab.
bool weekendHasStarted(const F1RaceWeekend& wk) {
  struct { const std::string& date; const std::string& time; } sessions[] = {
      {wk.fp1Date, wk.fp1Time},
      {wk.fp2Date, wk.fp2Time},
      {wk.fp3Date, wk.fp3Time},
      {wk.sprintQualDate, wk.sprintQualTime},
      {wk.sprintDate, wk.sprintTime},
      {wk.qualDate, wk.qualTime},
      {wk.raceDate, wk.raceTime},
  };
  for (const auto& s : sessions) {
    if (!s.date.empty()) return isSessionPast(s.date, s.time);
  }
  return false;
}

// Session sub-objects give date and time as two separate ISO fields ("date":
// "2026-03-06", "time": "01:30:00Z") rather than one combined timestamp like
// Football's dates — same UTC-offset shift and day-rollover-by-hand approach
// (mktime/timegm are deliberately avoided; they'd pull in libc's unconfigured
// timezone state on this target).
std::string formatSessionLocal(const std::string& isoDate, const std::string& isoTime) {
  if (isoDate.size() < 10 || isoTime.size() < 5) return "";
  int y = 0, mo = 0, d = 0;
  sscanf(isoDate.c_str(), "%d-%d-%d", &y, &mo, &d);
  int h = 0, mi = 0;
  sscanf(isoTime.c_str(), "%d:%d", &h, &mi);

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

  char buf[16];
  snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", mo, d, totalMinutes / 60, totalMinutes % 60);
  return buf;
}

// A driver's meaningful qualifying time is their last set lap — Q3 if they
// made it that far, else Q2, else Q1. Earlier segments aren't shown once a
// later one exists, same as how qualifying results are read on any timing
// screen.
std::string bestQualifyingTime(JsonObjectConst item) {
  std::string q3 = item["Q3"] | "";
  if (!q3.empty()) return q3;
  std::string q2 = item["Q2"] | "";
  if (!q2.empty()) return q2;
  return item["Q1"] | "";
}

// One cell of the driver detail's stat grid (see render()) — bold value over
// a small label, both centered, same layout idiom as BookStatsView's
// drawStatCell. An empty value (e.g. a historical/reserve driver with no
// permanent number or code) shows a dash instead of leaving the cell blank,
// so it reads as "not available" rather than looking broken.
void drawDriverStatCell(const GfxRenderer& renderer, int x, int y, int w, int h, const std::string& value,
                        const char* label) {
  const std::string shown = value.empty() ? "-" : value;
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int maxTextW = std::max(0, w - 12);
  const auto fittedValue = renderer.truncatedText(UI_12_FONT_ID, shown.c_str(), maxTextW, EpdFontFamily::BOLD);
  const auto fittedLabel = renderer.truncatedText(SMALL_FONT_ID, label, maxTextW);
  const int textY = y + std::max(0, (h - (valueLineH + 4 + labelLineH)) / 2);
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, fittedValue.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + (w - valueW) / 2, textY, fittedValue.c_str(), true, EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(SMALL_FONT_ID, fittedLabel.c_str());
  renderer.drawText(SMALL_FONT_ID, x + (w - labelW) / 2, textY + valueLineH + 4, fittedLabel.c_str(), true);
}

// The birth-date cell, drawn as its own three-line stack (date / label / age)
// instead of routing through drawDriverStatCell — appending the age to the
// date value on one line left it truncated off the edge of the half-width
// cell, unreadable. Putting it on its own line under the "Nacimiento" label
// keeps both the full date and the age legible.
void drawDriverBirthCell(const GfxRenderer& renderer, int x, int y, int w, int h, const std::string& date,
                         const std::string& age, const char* label) {
  const std::string shown = date.empty() ? "-" : date;
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int maxTextW = std::max(0, w - 12);
  const auto fittedValue = renderer.truncatedText(UI_12_FONT_ID, shown.c_str(), maxTextW, EpdFontFamily::BOLD);
  const auto fittedLabel = renderer.truncatedText(SMALL_FONT_ID, label, maxTextW);
  const std::string ageText = (date.empty() || age.empty()) ? "" : "(" + age + ")";
  const int totalH = valueLineH + 4 + smallLineH + (ageText.empty() ? 0 : 2 + smallLineH);
  const int textY = y + std::max(0, (h - totalH) / 2);

  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, fittedValue.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + (w - valueW) / 2, textY, fittedValue.c_str(), true, EpdFontFamily::BOLD);

  const int labelY = textY + valueLineH + 4;
  const int labelW = renderer.getTextWidth(SMALL_FONT_ID, fittedLabel.c_str());
  renderer.drawText(SMALL_FONT_ID, x + (w - labelW) / 2, labelY, fittedLabel.c_str(), true);

  if (!ageText.empty()) {
    const auto fittedAge = renderer.truncatedText(SMALL_FONT_ID, ageText.c_str(), maxTextW);
    const int ageW = renderer.getTextWidth(SMALL_FONT_ID, fittedAge.c_str());
    renderer.drawText(SMALL_FONT_ID, x + (w - ageW) / 2, labelY + smallLineH + 2, fittedAge.c_str(), true);
  }
}

// LOG_ERR/LOG_INF compile to nothing unless ENABLE_SERIAL_LOG is set, and even
// then need a serial monitor attached to read them. A bounded, stack-only
// line appended straight to SD survives both and needs no USB to inspect
// later - same fix FootballActivity::doFetch applied after hitting this in
// the field.
void writeDebugLine(const char* line) {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/f1");
  HalFile debugFile = Storage.open("/apps/f1/debug.log", O_WRITE | O_CREAT | O_APPEND);
  if (!debugFile) return;
  debugFile.write(line, strlen(line));
  debugFile.close();
}
}  // namespace

// Results normally means "the latest race" (selectedRound == -1), but
// drilling into a past race from the Calendar tab points it at a specific
// round instead — so, unlike the other tabs, its URL/cache path depend on
// more than just which tab this is.
std::string FormulaOneActivity::apiUrl(int tab) const {
  switch (static_cast<F1Tab>(tab)) {
    case F1Tab::Drivers:
      return "https://api.jolpi.ca/ergast/f1/current/driverstandings/";
    case F1Tab::Constructors:
      return "https://api.jolpi.ca/ergast/f1/current/constructorstandings/";
    case F1Tab::Results:
      if (selectedRound < 0) {
        return "https://api.jolpi.ca/ergast/f1/current/last/results/";
      }
      return "https://api.jolpi.ca/ergast/f1/current/" + std::to_string(selectedRound) + "/results/";
    case F1Tab::Qualifying:
      // Only ever reached with selectedRound already set by the Calendar
      // drill-in, unlike Results — no "last" fallback needed.
      return "https://api.jolpi.ca/ergast/f1/current/" + std::to_string(selectedRound) + "/qualifying/";
    case F1Tab::Sprint:
      return "https://api.jolpi.ca/ergast/f1/current/" + std::to_string(selectedRound) + "/sprint/";
    case F1Tab::SessionResult:
      // Never used directly — the alpha API has no single-request "round +
      // session" URL (an opaque round_id must be resolved first), so this
      // tab's fetch is a two-step flow handled entirely by
      // doFetchSessionResult() instead of the apiUrl()+downloadToFile()
      // one-shot every other tab uses. This case only exists so a bug that
      // routed SessionResult through the generic path fails loudly instead
      // of silently returning the Calendar URL via default:.
      return "";
    case F1Tab::Calendar:
    default:
      return "https://api.jolpi.ca/ergast/f1/current/races/";
  }
}

std::string FormulaOneActivity::cachePath(int tab) const {
  switch (static_cast<F1Tab>(tab)) {
    case F1Tab::Drivers:
      return "/apps/f1/drivers.json";
    case F1Tab::Constructors:
      return "/apps/f1/constructors.json";
    case F1Tab::Results:
      if (selectedRound < 0) return "/apps/f1/results_last.json";
      return "/apps/f1/results_r" + std::to_string(selectedRound) + ".json";
    case F1Tab::Qualifying:
      return "/apps/f1/qualifying_r" + std::to_string(selectedRound) + ".json";
    case F1Tab::Sprint:
      return "/apps/f1/sprint_r" + std::to_string(selectedRound) + ".json";
    case F1Tab::SessionResult:
      // Only the results (step 2) get cached — the resolved round_id (step
      // 1) is cheap/transient and only exists to build this path.
      return "/apps/f1/session_r" + std::to_string(selectedRound) + "_" + sessionResultCode + ".json";
    case F1Tab::Calendar:
    default:
      return "/apps/f1/calendar.json";
  }
}

void FormulaOneActivity::startFetch(int tab) {
  refreshing[tab] = true;
  refreshFailed[tab] = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/f1");
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this, tab](const ActivityResult& result) {
                              if (result.isCancelled) {
                                refreshing[tab] = false;
                                if (!loaded[tab]) {
                                  errorMessage[tab] = tr(STR_F1_WIFI_REQUIRED);
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

void FormulaOneActivity::doFetch(int tab) {
  requestUpdateAndWait();  // paint the "Refreshing..." state before the blocking call below
  wifiWasUsed = true;
  LOG_INF("F1", "doFetch tab=%d start: free=%u largest=%u", tab, (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());

  if (tab == static_cast<int>(F1Tab::SessionResult)) {
    // Two-step alpha-API flow (round_id lookup, then results) — doesn't fit
    // the single apiUrl()+downloadToFile() shot every other tab uses below.
    doFetchSessionResult();
    refreshing[tab] = false;
    requestUpdate();
    return;
  }

  // Written before the blocking call below, not just after: if this task gets
  // killed mid-fetch (TWDT reset, crash) instead of returning normally, the
  // post-fetch writeDebugLine() call further down never runs, and the debug
  // log otherwise stays completely silent about a failure that clearly
  // happened - see the "Client init failed" style reports that never showed
  // up in /apps/f1/debug.log. A "start" line with no matching "result" line
  // on the next boot is itself the diagnostic.
  {
    char startLine[128];
    const int len = snprintf(startLine, sizeof(startLine), "tab=%d fetch start heap free=%u largest=%u\n", tab,
                             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (len > 0) writeDebugLine(startLine);
  }

  // downloadToFile already writes destPath transactionally (hidden temp file,
  // verified size, atomic replace only on full success — see
  // NetworkFileTransaction in HttpDownloader.cpp) so the previous cache
  // survives untouched if this fails partway through. Downloading straight to
  // cachePath() avoids a second, unchecked manual rename on top of that,
  // which could silently leave stale data in place if it ever failed.
  const auto result = HttpDownloader::downloadToFile(apiUrl(tab), cachePath(tab));
  refreshing[tab] = false;

  // Free heap can look fine (60-70KB) while every fetch still fails, because
  // the app keeps every tab's parsed data resident for instant switching —
  // that residency leaves the heap fragmented into pieces too small for a
  // TLS handshake's contiguous buffer. Once that happens it doesn't recover on
  // its own: retrying just repeats the same failure (confirmed in the field —
  // three straight Qualifying attempts logging the identical stuck largest-
  // block value). silentRestart() is the same heap-defrag reboot every other
  // WiFi tool in this app already relies on when leaving; triggering it here
  // instead of leaving the user stuck on a permanently failing tab.
  if (result == HttpDownloader::HTTP_ERROR && ESP.getMaxAllocHeap() < kMinTlsContiguousHeap) {
    LOG_ERR("F1", "tab=%d download failed with fragmented heap (largest=%u) - restarting to recover", tab,
            (unsigned)ESP.getMaxAllocHeap());
    char debugLine[128];
    const int len = snprintf(debugLine, sizeof(debugLine), "tab=%d fragmented-heap restart (largest=%u)\n", tab,
                             (unsigned)ESP.getMaxAllocHeap());
    if (len > 0) writeDebugLine(debugLine);
    silentRestart();  // never returns unless a deep sleep is in progress
  }

  if (result == HttpDownloader::OK) {
    loadCacheFromSd(tab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      errorMessage[tab] = tr(STR_F1_NO_DATA);
    }
  } else if (!loaded[tab]) {
    errorMessage[tab] = tr(STR_F1_NO_DATA);
  } else {
    // Refresh failed but we still have good cached data from before — keep
    // showing it instead of an error screen, just flag it so render() can
    // surface a brief "couldn't update" banner instead of failing silently.
    refreshFailed[tab] = true;
  }

  if (result != HttpDownloader::OK) {
    char debugLine[220];
    const int len =
        snprintf(debugLine, sizeof(debugLine), "tab=%d result=%d (%s) heap free=%u largest=%u url=%s\n", tab,
                 static_cast<int>(result), HttpDownloader::lastErrorDetail(), (unsigned)ESP.getFreeHeap(),
                 (unsigned)ESP.getMaxAllocHeap(), apiUrl(tab).c_str());
    if (len > 0) writeDebugLine(debugLine);
  }
  LOG_INF("F1", "doFetch tab=%d done: free=%u largest=%u", tab, (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  requestUpdate();
}

// Two requests, not one: the alpha API has no "round + session" endpoint, so
// the round number this app already tracks must first be resolved to an
// opaque round_id before the actual results can be fetched. Step 1 goes
// through the same downloadToFile()-then-readFile() path every other fetch
// in this file uses (rather than fetching straight into a std::string) —
// consistent handling, and it leaves the raw response on disk to inspect if
// something goes wrong. It's a fixed, overwritten-every-time path (not
// keyed by round/session like cachePath()): step 1's result is only ever
// used immediately afterward within this same function, never reloaded from
// a stale cache the way step 2's result can be.
void FormulaOneActivity::doFetchSessionResult() {
  const int tab = static_cast<int>(F1Tab::SessionResult);
  static const char* kRoundLookupPath = "/apps/f1/round_lookup.json";
  LOG_INF("F1", "alpha step1 start: free=%u largest=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  const std::string roundsUrl = "https://api.jolpi.ca/f1/alpha/core/rounds/?year=" + sessionResultYear +
                                 "&round_number=" + std::to_string(selectedRound);
  if (HttpDownloader::downloadToFile(roundsUrl, kRoundLookupPath) != HttpDownloader::OK) {
    LOG_ERR("F1", "alpha round lookup download failed for year=%s round=%d", sessionResultYear.c_str(), selectedRound);
    if (!loaded[tab]) {
      errorMessage[tab] = tr(STR_F1_NO_DATA);
    } else {
      refreshFailed[tab] = true;
    }
    return;
  }
  String roundsInput = Storage.readFile(kRoundLookupPath);
  if (roundsInput.length() == 0) {
    LOG_ERR("F1", "alpha round lookup file empty for year=%s round=%d", sessionResultYear.c_str(), selectedRound);
    if (!loaded[tab]) {
      errorMessage[tab] = tr(STR_F1_NO_DATA);
    } else {
      refreshFailed[tab] = true;
    }
    return;
  }

  // Only the id is kept — same per-request filter idiom parseAndStore uses,
  // so the parsed DOM stays tiny regardless of response size.
  JsonDocument roundsFilter;
  roundsFilter["data"][0]["id"] = true;
  JsonDocument roundsDoc;
  // Same TWDT-feeding discipline as parseAndStore's deserializeJson — this
  // runs right after a TLS teardown, on the same fragmented-heap-prone path.
  esp_task_wdt_reset();
  const auto err = deserializeJson(roundsDoc, std::string(roundsInput.c_str()), DeserializationOption::Filter(roundsFilter));
  esp_task_wdt_reset();
  JsonArray data = roundsDoc["data"];
  if (err || data.size() == 0) {
    LOG_ERR("F1", "alpha round lookup failed for year=%s round=%d: %s", sessionResultYear.c_str(), selectedRound,
             err ? err.c_str() : "no matching round");
    if (!loaded[tab]) {
      errorMessage[tab] = tr(STR_F1_NO_DATA);
    } else {
      refreshFailed[tab] = true;
    }
    return;
  }
  f1AlphaRoundId = data[0]["id"] | "";
  if (f1AlphaRoundId.empty()) {
    if (!loaded[tab]) {
      errorMessage[tab] = tr(STR_F1_NO_DATA);
    } else {
      refreshFailed[tab] = true;
    }
    return;
  }
  LOG_INF("F1", "alpha step1 done (round_id=%s): free=%u largest=%u", f1AlphaRoundId.c_str(),
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  const auto dlResult = HttpDownloader::downloadToFile(
      "https://api.jolpi.ca/f1/alpha/results/" + f1AlphaRoundId + "/" + sessionResultCode + "/", cachePath(tab));
  if (dlResult == HttpDownloader::OK) {
    loadCacheFromSd(tab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      errorMessage[tab] = tr(STR_F1_NO_DATA);
    }
  } else if (!loaded[tab]) {
    errorMessage[tab] = tr(STR_F1_NO_DATA);
  } else {
    refreshFailed[tab] = true;
  }
  LOG_INF("F1", "alpha step2 done: free=%u largest=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

bool FormulaOneActivity::loadCacheFromSd(int tab) {
  String input = Storage.readFile(cachePath(tab).c_str());
  if (input.length() == 0) return false;
  parseAndStore(tab, std::string(input.c_str()));
  return loaded[tab];
}

void FormulaOneActivity::evictTabData(int tab) {
  if (!loaded[tab]) return;  // nothing resident to free
  rows[tab].clear();
  rows[tab].shrink_to_fit();
  if (tab == static_cast<int>(F1Tab::Drivers)) {
    driverBios.clear();
    driverBios.shrink_to_fit();
  }
  loaded[tab] = false;
}

void FormulaOneActivity::parseAndStore(int tab, const std::string& json) {
  // Only keep the fields we actually render — cuts ArduinoJson's parsed-tree
  // memory well below what the full response (URLs, dates of birth, lap
  // times, etc.) would need, which matters a lot on this hardware's RAM.
  JsonDocument filter;
  if (tab == static_cast<int>(F1Tab::Drivers)) {
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["position"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["points"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["givenName"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["familyName"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["permanentNumber"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["code"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["url"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["dateOfBirth"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["nationality"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Constructors"][0]["name"] = true;
  } else if (tab == static_cast<int>(F1Tab::Constructors)) {
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["position"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["points"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["wins"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["Constructor"]["name"] = true;
  } else if (tab == static_cast<int>(F1Tab::Results)) {
    filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["position"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["points"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Driver"]["familyName"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"] = true;
  } else if (tab == static_cast<int>(F1Tab::Qualifying)) {
    filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["QualifyingResults"][0]["position"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["QualifyingResults"][0]["Driver"]["familyName"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["QualifyingResults"][0]["Q1"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["QualifyingResults"][0]["Q2"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["QualifyingResults"][0]["Q3"] = true;
  } else if (tab == static_cast<int>(F1Tab::Sprint)) {
    filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["SprintResults"][0]["position"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["SprintResults"][0]["points"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["SprintResults"][0]["Driver"]["familyName"] = true;
  } else if (tab == static_cast<int>(F1Tab::SessionResult)) {
    // The alpha API returns typed JSON (unlike Ergast, where every field is
    // a string) — position_text is the string form, used here instead of
    // the int-typed position so the existing `| ""` idiom below stays valid.
    filter["data"]["results"][0]["position_text"] = true;
    filter["data"]["results"][0]["driver"]["family_name"] = true;
    filter["data"]["results"][0]["time"] = true;
  } else {
    // Calendar: the [0] index below is an ArduinoJson filter wildcard — it
    // applies to every entry in Races[], not just the first one, since we
    // want the whole season's schedule rather than a single race.
    filter["MRData"]["RaceTable"]["Races"][0]["round"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["time"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Circuit"]["circuitName"] = true;
    // Session schedule for races that haven't run yet (shown from Calendar
    // instead of results). SprintQualifying/SprintShootout: Ergast/Jolpica
    // has used both names for this session across seasons — filtering both
    // is harmless (an absent field just yields nothing) and future-proofs
    // against whichever name the current season uses.
    filter["MRData"]["RaceTable"]["Races"][0]["FirstPractice"]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["FirstPractice"]["time"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["SecondPractice"]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["SecondPractice"]["time"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["ThirdPractice"]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["ThirdPractice"]["time"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Qualifying"]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Qualifying"]["time"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Sprint"]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Sprint"]["time"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["SprintQualifying"]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["SprintQualifying"]["time"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["SprintShootout"]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["SprintShootout"]["time"] = true;
  }

  // HttpDownloader.cpp's own read loop resets the task watchdog because a
  // slow multi-minute download can otherwise trip it (loopTask is on the
  // TWDT) - deserializeJson() below has no such reset. Parsing a fragmented
  // heap can force ArduinoJson to grow its pool many times over, and a
  // deeply-nested response (Results, with a Driver+Constructor object per
  // row) does more of that than a flatter one of similar byte size. Feeding
  // it immediately before matches the same discipline for the same reason.
  esp_task_wdt_reset();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  esp_task_wdt_reset();
  if (err) {
    LOG_ERR("F1", "JSON parse failed for tab %d (%u bytes): %s", tab, static_cast<unsigned>(json.size()), err.c_str());
    char debugLine[128];
    const int len = snprintf(debugLine, sizeof(debugLine), "tab=%d JSON parse failed (%u bytes): %s\n", tab,
                             static_cast<unsigned>(json.size()), err.c_str());
    if (len > 0) writeDebugLine(debugLine);
    errorMessage[tab] = tr(STR_F1_NO_DATA);
    return;
  }

  std::vector<F1Row> newRows;
  std::vector<int> newCalendarRounds;               // only populated for tab == Calendar
  std::vector<F1RaceWeekend> newCalendarWeekends;    // same
  std::vector<F1DriverBio> newDriverBios;            // only populated for tab == Drivers

  if (tab == static_cast<int>(F1Tab::Drivers)) {
    JsonArray standings = doc["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"];
    newRows.reserve(standings.size());
    newDriverBios.reserve(standings.size());
    for (JsonObject item : standings) {
      std::string pos = item["position"] | "";
      std::string given = item["Driver"]["givenName"] | "";
      std::string family = item["Driver"]["familyName"] | "";
      std::string team = item["Constructors"][0]["name"] | "";
      std::string points = item["points"] | "0";
      newRows.push_back(F1Row{pos + ". " + given + " " + family, team, points});
      newDriverBios.push_back(F1DriverBio{item["Driver"]["permanentNumber"] | "", item["Driver"]["code"] | "",
                                          item["Driver"]["nationality"] | "", item["Driver"]["dateOfBirth"] | "",
                                          item["Driver"]["url"] | ""});
    }
  } else if (tab == static_cast<int>(F1Tab::Constructors)) {
    JsonArray standings = doc["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"];
    newRows.reserve(standings.size());
    for (JsonObject item : standings) {
      std::string pos = item["position"] | "";
      std::string name = item["Constructor"]["name"] | "";
      std::string wins = item["wins"] | "0";
      std::string points = item["points"] | "0";
      newRows.push_back(F1Row{pos + ". " + name, wins + " wins", points});
    }
  } else if (tab == static_cast<int>(F1Tab::Results)) {
    JsonObject race = doc["MRData"]["RaceTable"]["Races"][0];
    raceName = race["raceName"] | "";
    raceDate = race["date"] | "";
    JsonArray results = race["Results"];
    newRows.reserve(results.size());
    for (JsonObject item : results) {
      std::string pos = item["position"] | "";
      std::string family = item["Driver"]["familyName"] | "";
      std::string team = item["Constructor"]["name"] | "";
      std::string points = item["points"] | "0";
      newRows.push_back(F1Row{pos + ". " + family, team, points});
    }
  } else if (tab == static_cast<int>(F1Tab::Qualifying)) {
    JsonObject race = doc["MRData"]["RaceTable"]["Races"][0];
    raceName = race["raceName"] | "";
    raceDate = race["date"] | "";
    JsonArray results = race["QualifyingResults"];
    newRows.reserve(results.size());
    for (JsonObject item : results) {
      std::string pos = item["position"] | "";
      std::string family = item["Driver"]["familyName"] | "";
      newRows.push_back(F1Row{pos + ". " + family, "", bestQualifyingTime(item)});
    }
  } else if (tab == static_cast<int>(F1Tab::Sprint)) {
    JsonObject race = doc["MRData"]["RaceTable"]["Races"][0];
    raceName = race["raceName"] | "";
    raceDate = race["date"] | "";
    JsonArray results = race["SprintResults"];
    newRows.reserve(results.size());
    for (JsonObject item : results) {
      std::string pos = item["position"] | "";
      std::string family = item["Driver"]["familyName"] | "";
      std::string points = item["points"] | "0";
      newRows.push_back(F1Row{pos + ". " + family, "", points});
    }
  } else if (tab == static_cast<int>(F1Tab::SessionResult)) {
    // No raceName/raceDate here (unlike Results/Qualifying/Sprint) — the
    // alpha response's round/circuit fields aren't in the filter above,
    // since render() shows sessionResultLabel as the subtitle instead of a
    // date for this tab. For SQ specifically, "time" is already the driver's
    // best-of-SQ1/SQ2/SQ3 time resolved server-side, same shortcut
    // bestQualifyingTime() gives Q1/Q2/Q3 above — no segment fallback logic
    // needed here.
    JsonArray results = doc["data"]["results"];
    newRows.reserve(results.size());
    for (JsonObject item : results) {
      std::string pos = item["position_text"] | "";
      std::string family = item["driver"]["family_name"] | "";
      std::string time = item["time"] | "";
      newRows.push_back(F1Row{pos + ". " + family, "", time});
    }
  } else {
    JsonArray races = doc["MRData"]["RaceTable"]["Races"];
    newRows.reserve(races.size());
    newCalendarRounds.reserve(races.size());
    for (JsonObject item : races) {
      std::string roundStr = item["round"] | "";
      int round = roundStr.empty() ? 0 : atoi(roundStr.c_str());
      std::string name = item["raceName"] | "";
      std::string circuit = item["Circuit"]["circuitName"] | "";
      std::string date = item["date"] | "";
      newRows.push_back(F1Row{std::to_string(round) + ". " + name, circuit, shortDate(date)});
      newCalendarRounds.push_back(round);

      F1RaceWeekend wk;
      wk.raceDate = date;
      wk.raceTime = item["time"] | "";
      wk.fp1Date = item["FirstPractice"]["date"] | "";
      wk.fp1Time = item["FirstPractice"]["time"] | "";
      wk.fp2Date = item["SecondPractice"]["date"] | "";
      wk.fp2Time = item["SecondPractice"]["time"] | "";
      wk.fp3Date = item["ThirdPractice"]["date"] | "";
      wk.fp3Time = item["ThirdPractice"]["time"] | "";
      wk.qualDate = item["Qualifying"]["date"] | "";
      wk.qualTime = item["Qualifying"]["time"] | "";
      wk.sprintDate = item["Sprint"]["date"] | "";
      wk.sprintTime = item["Sprint"]["time"] | "";
      wk.sprintQualDate = item["SprintQualifying"]["date"] | "";
      wk.sprintQualTime = item["SprintQualifying"]["time"] | "";
      if (wk.sprintQualDate.empty()) {
        wk.sprintQualDate = item["SprintShootout"]["date"] | "";
        wk.sprintQualTime = item["SprintShootout"]["time"] | "";
      }
      newCalendarWeekends.push_back(std::move(wk));
    }
  }

  if (newRows.empty()) {
    LOG_ERR("F1", "Parsed JSON for tab %d but found 0 rows (unexpected shape?)", tab);
    // SessionResult specifically: the device's clock can mark a session as
    // past before jolpica-f1 has actually ingested its results yet — a 200
    // OK with an empty results[] rather than a parse failure, so it gets its
    // own message instead of the generic "could not load".
    errorMessage[tab] =
        (tab == static_cast<int>(F1Tab::SessionResult)) ? tr(STR_F1_SESSION_NO_RESULTS_YET) : tr(STR_F1_NO_DATA);
    return;
  }

  rows[tab] = std::move(newRows);
  loaded[tab] = true;
  errorMessage[tab].clear();
  if (tab == static_cast<int>(F1Tab::Drivers)) {
    driverBios = std::move(newDriverBios);
  } else if (tab == static_cast<int>(F1Tab::Calendar)) {
    calendarRounds = std::move(newCalendarRounds);
    calendarWeekends = std::move(newCalendarWeekends);
    positionCalendarCursorOnLastPastRace();
  }
}

// One-shot: the first time the Calendar loads, jump its cursor to the most
// recent weekend showing "Ver" (see weekendHasStarted/render()'s valueFn) —
// the last one with something actually viewable — rather than the last
// *race*, which lags behind by a whole weekend once that weekend's practice/
// qualifying/sprint sessions have started but the race itself hasn't run yet.
void FormulaOneActivity::positionCalendarCursorOnLastPastRace() {
  if (calendarCursorPositioned) return;

  int idx = -1;
  for (size_t i = 0; i < calendarWeekends.size(); i++) {
    if (weekendHasStarted(calendarWeekends[i])) idx = static_cast<int>(i);
  }
  if (idx < 0) return;  // no started weekend yet (clock not synced, or season hasn't started) — try again next parse

  selectedRow[static_cast<int>(F1Tab::Calendar)] = idx;
  calendarCursorPositioned = true;
}

// No network fetch needed — everything the detail view shows (number, code,
// nationality, date of birth, points) already came in with the Drivers tab's
// own fetch, parallel in driverBios/rows[Drivers] (see F1DriverBio). The QR
// button (see loop()/render()) reuses driverBios[index].wikiUrl for anyone
// who wants more than the API gives, without the device itself fetching
// Wikipedia.
void FormulaOneActivity::openDriverDetail(int index) {
  const auto& driverRows = rows[static_cast<int>(F1Tab::Drivers)];
  if (index < 0 || index >= static_cast<int>(driverRows.size()) || index >= static_cast<int>(driverBios.size())) {
    return;
  }
  detailDriverIndex = index;
  showingDriverDetail = true;
  requestUpdate();
}

// Chronological order for a normal weekend: FP1, FP2, FP3, Qualifying, Race.
// On a sprint weekend FP2/FP3 are simply absent (addSession skips empty
// dates), so the same call order naturally comes out as FP1, Sprint
// Qualifying, Sprint, Qualifying, Race — which is also the real chronological
// order there.
void FormulaOneActivity::showSessionSchedule(int calendarIdx) {
  const auto& wk = calendarWeekends[calendarIdx];
  std::vector<F1Row> sessionRows;
  std::vector<F1SessionScheduleTarget> targets;

  // target is the tab this session's results live in — F1Tab::SessionSchedule
  // itself means "not viewable yet" (session hasn't happened). A row only
  // becomes a "Ver" once its own time has passed, not just the day.
  // alphaCode is only meaningful when target == F1Tab::SessionResult (the
  // jolpica-f1 alpha session_filter code to fetch); pass nullptr otherwise.
  auto addSession = [&](const char* label, const std::string& date, const std::string& time, F1Tab target,
                        const char* alphaCode) {
    if (date.empty()) return;
    const bool viewable = target != F1Tab::SessionSchedule && isSessionPast(date, time);
    sessionRows.push_back(F1Row{label, "", viewable ? std::string(tr(STR_F1_VIEW_RESULT)) : formatSessionLocal(date, time)});
    targets.push_back(viewable ? F1SessionScheduleTarget{target, alphaCode ? alphaCode : "", label}
                                : F1SessionScheduleTarget{F1Tab::SessionSchedule, "", ""});
  };
  addSession(tr(STR_F1_SESSION_FP1), wk.fp1Date, wk.fp1Time, F1Tab::SessionResult, "FP1");
  addSession(tr(STR_F1_SESSION_FP2), wk.fp2Date, wk.fp2Time, F1Tab::SessionResult, "FP2");
  addSession(tr(STR_F1_SESSION_FP3), wk.fp3Date, wk.fp3Time, F1Tab::SessionResult, "FP3");
  addSession(tr(STR_F1_SESSION_SPRINT_QUAL), wk.sprintQualDate, wk.sprintQualTime, F1Tab::SessionResult, "SQ");
  addSession(tr(STR_F1_SESSION_SPRINT), wk.sprintDate, wk.sprintTime, F1Tab::Sprint, nullptr);
  addSession(tr(STR_F1_SESSION_QUALIFYING), wk.qualDate, wk.qualTime, F1Tab::Qualifying, nullptr);
  addSession(tr(STR_F1_SESSION_RACE), wk.raceDate, wk.raceTime, F1Tab::Results, nullptr);

  const int scheduleTab = static_cast<int>(F1Tab::SessionSchedule);
  rows[scheduleTab] = std::move(sessionRows);
  sessionScheduleTargets = std::move(targets);
  loaded[scheduleTab] = true;
  errorMessage[scheduleTab].clear();
  selectedRow[scheduleTab] = 0;

  const int calTab = static_cast<int>(F1Tab::Calendar);
  sessionScheduleRaceName =
      (calendarIdx >= 0 && calendarIdx < static_cast<int>(rows[calTab].size())) ? rows[calTab][calendarIdx].title : "";
  sessionScheduleCalendarIdx = calendarIdx;

  currentTab = F1Tab::SessionSchedule;
}

// Shared by enterSessionResults/enterAlphaSessionResult: the other
// session-detail tabs aren't fetched here — only the one the user actually
// asked to see. Confirming a different session later goes back through
// SessionSchedule, which re-enters for that one specifically.
// raceName/raceDate are shared (not per-tab) subheader state, so they're
// cleared here too — otherwise a failed or slow fetch for this round would
// leave the previous round's name showing above an empty/error body.
void FormulaOneActivity::clearSessionDetailTabsState() {
  for (F1Tab detailTab : {F1Tab::Results, F1Tab::Qualifying, F1Tab::Sprint, F1Tab::SessionResult}) {
    const int t = static_cast<int>(detailTab);
    loaded[t] = false;
    rows[t].clear();
    errorMessage[t].clear();
  }
  raceName.clear();
  raceDate.clear();
}

// Confirming a SessionSchedule "Ver" row drills into that one session's
// results (whichever of Results/Qualifying/Sprint it is) - Calendar confirm
// always opens the schedule first now (see loop()), so this is the only way
// in, and Back out of here always returns to that schedule.
void FormulaOneActivity::enterSessionResults(F1Tab targetTab, int round) {
  selectedRound = round;
  currentTab = targetTab;
  clearSessionDetailTabsState();
  const int t = static_cast<int>(targetTab);
  if (!loadCacheFromSd(t)) {
    startFetch(t);
  }
  requestUpdate();
}

// Same entry point as enterSessionResults, but for a Practice/Sprint-
// Qualifying row: those share the single SessionResult tab, parameterized
// per entry by which alpha session to fetch (see doFetchSessionResult).
void FormulaOneActivity::enterAlphaSessionResult(int round, const std::string& alphaCode, const std::string& label) {
  selectedRound = round;
  currentTab = F1Tab::SessionResult;
  sessionResultCode = alphaCode;
  sessionResultLabel = label;
  f1AlphaRoundId.clear();
  // Year isn't tracked as standalone state elsewhere in this file (round-
  // based Ergast URLs use the literal "current") — every session in a
  // weekend shares one year, so derive it from this weekend's own race date.
  sessionResultYear = (sessionScheduleCalendarIdx >= 0 &&
                        sessionScheduleCalendarIdx < static_cast<int>(calendarWeekends.size()) &&
                        calendarWeekends[sessionScheduleCalendarIdx].raceDate.size() >= 4)
                           ? calendarWeekends[sessionScheduleCalendarIdx].raceDate.substr(0, 4)
                           : "";

  clearSessionDetailTabsState();
  raceName = sessionScheduleRaceName;  // subheader title; subtitle is sessionResultLabel, see render()

  const int t = static_cast<int>(F1Tab::SessionResult);
  if (!loadCacheFromSd(t)) {
    startFetch(t);
  }
  requestUpdate();
}

void FormulaOneActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("F1", "onEnter start: free=%u largest=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // Only the tab actually starting on screen gets loaded here. Eagerly
  // parsing all 8 tabs upfront kept every one of them resident in RAM for
  // the whole session regardless of whether the user ever visited most of
  // them — that permanent residency is what fragmented the heap badly enough
  // to fail a later TLS handshake even with tens of KB still nominally free
  // (see evictTabData()/doFetch()'s fragmented-heap restart). Left/Right and
  // the drill-down entry points already lazy-load whichever tab they switch
  // to, same as this does for the starting one. SessionSchedule is skipped —
  // it's never fetched/cached on its own, only built in-memory from the
  // Calendar's own data (see showSessionSchedule()), and currentTab is never
  // SessionSchedule this early anyway.
  const int startTab = static_cast<int>(currentTab);
  if (currentTab != F1Tab::SessionSchedule && !loaded[startTab]) {
    if (!loadCacheFromSd(startTab)) {
      startFetch(startTab);
    }
  }
  LOG_INF("F1", "onEnter after cache load: free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());

  requestUpdate();
}

void FormulaOneActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void FormulaOneActivity::loop() {
  // Driver detail is a small overlay on top of the Drivers tab, not a real
  // F1Tab — it owns Back/Confirm outright while open and nothing below this
  // block runs, same as how QrDisplayActivity itself takes over the screen.
  if (showingDriverDetail) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      showingDriverDetail = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && detailDriverIndex >= 0 &&
        detailDriverIndex < static_cast<int>(driverBios.size()) && !driverBios[detailDriverIndex].wikiUrl.empty()) {
      startActivityForResult(
          makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, driverBios[detailDriverIndex].wikiUrl),
          [this](const ActivityResult&) { requestUpdate(); });
    }
    return;
  }

  // Drivers: hold Confirm to refresh. Every other button on this tab is
  // already spoken for (Left/Right switch tabs, Up/Down scroll, and a plain
  // tap of Confirm now opens the selected driver's detail below), so the
  // manual refresh moves to a hold instead of a dedicated key — same idiom
  // as CalendarActivity's hold-for-Holidays. The "fired" flag swallows the
  // release that ends the hold so it doesn't also open the detail view for
  // whatever row is under the cursor.
  if (driverRefreshHoldFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      driverRefreshHoldFired = false;
    }
    return;
  }
  if (currentTab == F1Tab::Drivers && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= HoldGestures::SHORT_MS) {
    driverRefreshHoldFired = true;
    startFetch(static_cast<int>(F1Tab::Drivers));
    return;
  }

  // Calendar: same hold-Confirm-to-refresh idiom as Drivers above — tap is
  // already spoken for (opens SessionSchedule).
  if (calendarRefreshHoldFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      calendarRefreshHoldFired = false;
    }
    return;
  }
  if (currentTab == F1Tab::Calendar && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= HoldGestures::SHORT_MS) {
    calendarRefreshHoldFired = true;
    startFetch(static_cast<int>(F1Tab::Calendar));
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Drilled into a past race from the Calendar tab: Back returns to the
    // calendar instead of exiting. Nothing here needs reloading - Results
    // isn't on screen once we're back on Calendar, so there's no reason to
    // fetch anything (that used to eagerly reload "last race" into the
    // shared raceName/raceDate members, which then stuck around and got
    // shown as a stale subheader the next time a *different* round's fetch
    // was slow or failed - looking like results were always for one fixed
    // race no matter which one got picked).
    if ((currentTab == F1Tab::Results || currentTab == F1Tab::Qualifying || currentTab == F1Tab::Sprint ||
         currentTab == F1Tab::SessionResult) &&
        selectedRound >= 0) {
      selectedRound = -1;
      // Always reached via a SessionSchedule "Ver" row (Calendar confirm
      // opens the schedule first, see below) — Back always returns there.
      if (sessionScheduleCalendarIdx >= 0 && sessionScheduleCalendarIdx < static_cast<int>(calendarWeekends.size())) {
        showSessionSchedule(sessionScheduleCalendarIdx);
      } else {
        currentTab = F1Tab::Calendar;  // defensive fallback, shouldn't happen
      }
      requestUpdate();
      return;
    }
    // Session schedule is also reached only from Calendar — same "back to
    // parent" treatment, just without anything to cancel/reload since
    // showing it never involved a fetch in the first place. Whichever detail
    // tab (Results/Qualifying/Sprint/SessionResult) was last viewed for this
    // weekend would otherwise sit resident in RAM for the rest of the
    // session — nothing else frees it until a *different* one is opened (see
    // clearSessionDetailTabsState()'s own callers).
    if (currentTab == F1Tab::SessionSchedule) {
      clearSessionDetailTabsState();
      currentTab = F1Tab::Calendar;
      requestUpdate();
      return;
    }
    onGoHome(HomeMenuItem::APPS_MENU);
    return;
  }

  // Results/Qualifying/Sprint/SessionSchedule aren't real tabs — they're only
  // reached by drilling into a Calendar row — so Left/Right on the main bar
  // only cycle the 3 tabs actually in it. None of the four have anything for
  // Left/Right to do: the session-results views scroll with Up/Down only,
  // and SessionSchedule is a static list of times.
  const bool inDetailView = (currentTab == F1Tab::Results || currentTab == F1Tab::Qualifying ||
                              currentTab == F1Tab::Sprint || currentTab == F1Tab::SessionSchedule ||
                              currentTab == F1Tab::SessionResult);
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && !inDetailView) {
    const int leavingTab = static_cast<int>(currentTab);
    if (currentTab == F1Tab::Drivers) {
      currentTab = F1Tab::Calendar;
    } else if (currentTab == F1Tab::Constructors) {
      currentTab = F1Tab::Drivers;
    } else {
      currentTab = F1Tab::Constructors;  // was Calendar
    }
    evictTabData(leavingTab);
    int tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty() && !loadCacheFromSd(tab)) {
      startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !inDetailView) {
    const int leavingTab = static_cast<int>(currentTab);
    if (currentTab == F1Tab::Drivers) {
      currentTab = F1Tab::Constructors;
    } else if (currentTab == F1Tab::Constructors) {
      currentTab = F1Tab::Calendar;
    } else {
      currentTab = F1Tab::Drivers;  // was Calendar
    }
    evictTabData(leavingTab);
    int tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty() && !loadCacheFromSd(tab)) {
      startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    int tab = static_cast<int>(currentTab);
    if (!rows[tab].empty()) {
      selectedRow[tab] = static_cast<int>((selectedRow[tab] - 1 + rows[tab].size()) % rows[tab].size());
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    int tab = static_cast<int>(currentTab);
    if (!rows[tab].empty()) {
      selectedRow[tab] = static_cast<int>((selectedRow[tab] + 1) % rows[tab].size());
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (currentTab == F1Tab::Calendar) {
      // Always opens the session schedule, whether the race already ran or
      // not — it lists every session either way, formatted time if it
      // hasn't happened yet or "Ver" once it has (see showSessionSchedule()).
      // The loaded[] check matters now that evictTabData()/a failed reload
      // can leave rows[Calendar] empty while calendarWeekends still holds an
      // older, evicted-away tab's stale contents — without it, a Confirm
      // pressed during that brief error state could open a schedule that
      // doesn't match whatever failed to (re)load.
      const int idx = selectedRow[static_cast<int>(F1Tab::Calendar)];
      if (loaded[static_cast<int>(F1Tab::Calendar)] && idx >= 0 && idx < static_cast<int>(calendarWeekends.size())) {
        showSessionSchedule(idx);
        requestUpdate();
      }
    } else if (currentTab == F1Tab::Drivers) {
      openDriverDetail(selectedRow[static_cast<int>(F1Tab::Drivers)]);
    } else if (currentTab == F1Tab::SessionSchedule) {
      const int idx = selectedRow[static_cast<int>(F1Tab::SessionSchedule)];
      if (idx >= 0 && idx < static_cast<int>(sessionScheduleTargets.size()) &&
          sessionScheduleTargets[idx].tab != F1Tab::SessionSchedule && sessionScheduleCalendarIdx >= 0 &&
          sessionScheduleCalendarIdx < static_cast<int>(calendarRounds.size())) {
        const auto& target = sessionScheduleTargets[idx];
        const int round = calendarRounds[sessionScheduleCalendarIdx];
        if (target.tab == F1Tab::SessionResult) {
          enterAlphaSessionResult(round, target.alphaCode, target.label);
        } else {
          enterSessionResults(target.tab, round);
        }
      }
    } else {
      startFetch(static_cast<int>(currentTab));
    }
  }
}

void FormulaOneActivity::drawTabStrip(int y, const std::vector<std::string>& labels, int selectedIndex) {
  const auto pageWidth = renderer.getScreenWidth();
  const int count = static_cast<int>(labels.size());
  const int tabW = (pageWidth - 40) / count;
  constexpr int kTabH = 30;
  // A fixed "y + 7" offset assumed a specific line height; centering on the
  // font's actual line height (same as FootballActivity::drawTabStrip) keeps
  // the label vertically centered regardless of font metrics.
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

void FormulaOneActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_F1_TITLE));

  if (showingDriverDetail && detailDriverIndex >= 0 &&
      detailDriverIndex < static_cast<int>(rows[static_cast<int>(F1Tab::Drivers)].size()) &&
      detailDriverIndex < static_cast<int>(driverBios.size())) {
    const auto& driverRow = rows[static_cast<int>(F1Tab::Drivers)][detailDriverIndex];
    const auto& bio = driverBios[detailDriverIndex];

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight}, driverRow.title.c_str(),
                       driverRow.subtitle.c_str());

    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

    // A bordered card of stat cells — everything here is already sitting in
    // driverBios/driverRow from the Drivers tab's own fetch, no separate
    // request needed. 3 cells on top (number/code/nationality), 2 wider ones
    // below (birth date/points), same grid idiom as BookStatsView's stat
    // cards elsewhere in the app.
    const int cardX = metrics.contentSidePadding;
    const int cardW = pageWidth - 2 * metrics.contentSidePadding;
    const int cardY = contentTop + metrics.subHeaderHeight + metrics.verticalSpacing;
    const int cardH = listBottom - cardY;
    const int rowH = cardH / 2;
    const int thirdW = cardW / 3;
    const int halfW = cardW / 2;

    renderer.drawRect(cardX, cardY, cardW, cardH);
    renderer.drawLine(cardX, cardY + rowH, cardX + cardW, cardY + rowH);
    renderer.drawLine(cardX + thirdW, cardY, cardX + thirdW, cardY + rowH);
    renderer.drawLine(cardX + thirdW * 2, cardY, cardX + thirdW * 2, cardY + rowH);
    renderer.drawLine(cardX + halfW, cardY + rowH, cardX + halfW, cardY + cardH);

    drawDriverStatCell(renderer, cardX, cardY, thirdW, rowH, bio.number.empty() ? "" : "#" + bio.number,
                       tr(STR_F1_STAT_NUMBER));
    drawDriverStatCell(renderer, cardX + thirdW, cardY, thirdW, rowH, bio.code, tr(STR_F1_STAT_CODE));
    drawDriverStatCell(renderer, cardX + thirdW * 2, cardY, cardW - thirdW * 2, rowH, bio.nationality,
                       tr(STR_F1_STAT_NATIONALITY));
    drawDriverBirthCell(renderer, cardX, cardY + rowH, halfW, cardH - rowH, fullDate(bio.dateOfBirth),
                        computeAge(bio.dateOfBirth), tr(STR_F1_STAT_BIRTH));
    drawDriverStatCell(renderer, cardX + halfW, cardY + rowH, cardW - halfW, cardH - rowH, driverRow.value,
                       tr(STR_F1_STAT_POINTS));

    const char* qrHint = bio.wikiUrl.empty() ? nullptr : tr(STR_F1_SHOW_QR);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), qrHint, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Results/Qualifying/Sprint/SessionSchedule aren't in the main tab bar —
  // they're detail views reached by drilling into a Calendar row.
  // SessionSchedule keeps the main bar frozen on Calendar (its parent); the
  // session-results views drop the tab strip entirely instead of the old
  // Left/Right mini-strip — Back returns straight to SessionSchedule to pick
  // a different session (see loop()).
  const bool inSessionResultsView = (currentTab == F1Tab::Results || currentTab == F1Tab::Qualifying ||
                                     currentTab == F1Tab::Sprint || currentTab == F1Tab::SessionResult);
  const int tabBarY = metrics.topPadding + metrics.headerHeight + 20;
  int contentTop = tabBarY;
  if (!inSessionResultsView) {
    const std::vector<std::string> tabLabels = {tr(STR_F1_TAB_DRIVERS), tr(STR_F1_TAB_CONSTRUCTORS),
                                                 tr(STR_F1_TAB_CALENDAR)};
    int selectedTabIndex = 2;  // Calendar
    if (currentTab == F1Tab::Drivers) selectedTabIndex = 0;
    else if (currentTab == F1Tab::Constructors) selectedTabIndex = 1;
    drawTabStrip(tabBarY, tabLabels, selectedTabIndex);
    contentTop = tabBarY + 30 + metrics.verticalSpacing;
  }

  if (inSessionResultsView && !raceName.empty()) {
    // SessionResult has no single date the way Results/Qualifying/Sprint do
    // (it's ambiguous which session), so its subtitle is the session label
    // (e.g. "Practice 1") instead of a date.
    const std::string subtitle =
        (currentTab == F1Tab::SessionResult) ? sessionResultLabel : shortDate(raceDate);
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight}, raceName.c_str(),
                       subtitle.c_str());
    contentTop += metrics.subHeaderHeight + metrics.verticalSpacing;
  } else if (currentTab == F1Tab::SessionSchedule) {
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight}, sessionScheduleRaceName.c_str(),
                       nullptr);
    contentTop += metrics.subHeaderHeight + metrics.verticalSpacing;
  }

  const int tab = static_cast<int>(currentTab);
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (!loaded[tab] && !refreshing[tab]) {
    int textY = contentTop + (listBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    const char* msg = !errorMessage[tab].empty() ? errorMessage[tab].c_str() : tr(STR_F1_LOADING);
    renderer.drawCenteredText(UI_12_FONT_ID, textY, msg);
  } else {
    const auto& tabRows = rows[tab];
    // Results/Qualifying/Sprint skip the team subtitle so rows use the
    // shorter row height and more drivers fit on screen at once; team name
    // isn't essential there since it's already shown in the Drivers
    // standings tab.
    std::function<std::string(int)> subtitleFn;
    if (!inSessionResultsView) {
      subtitleFn = [&tabRows](int i) { return tabRows[i].subtitle; };
    }
    // Calendar rows show "Ver" once the weekend has started instead of the
    // baked-in date — computed fresh here (not at parse time) since it
    // depends on wall-clock time, which keeps moving while the app sits on
    // another tab. Every other tab's value column is just the parsed value.
    std::function<std::string(int)> valueFn = [&tabRows](int i) { return tabRows[i].value; };
    if (currentTab == F1Tab::Calendar) {
      valueFn = [this, &tabRows](int i) {
        if (i >= 0 && i < static_cast<int>(calendarWeekends.size()) && weekendHasStarted(calendarWeekends[i])) {
          return std::string(tr(STR_F1_VIEW_RESULT));
        }
        return tabRows[i].value;
      };
    }
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, listBottom - contentTop}, static_cast<int>(tabRows.size()),
        selectedRow[tab], [&tabRows](int i) { return tabRows[i].title; }, subtitleFn, nullptr, valueFn, true);
  }

  if (refreshing[tab]) {
    GUI.drawPopup(renderer, tr(STR_F1_REFRESHING));
  } else if (refreshFailed[tab]) {
    GUI.drawPopup(renderer, tr(STR_F1_REFRESH_FAILED));
  }

  const char* confirmHint;
  if (currentTab == F1Tab::Calendar) {
    confirmHint = tr(STR_SELECT);
  } else if (currentTab == F1Tab::SessionSchedule) {
    // Only rows already showing "Ver" (see showSessionSchedule) are
    // confirmable — everything else is a plain time with nothing to view yet.
    const int idx = selectedRow[tab];
    const bool viewable = idx >= 0 && idx < static_cast<int>(sessionScheduleTargets.size()) &&
                           sessionScheduleTargets[idx].tab != F1Tab::SessionSchedule;
    confirmHint = viewable ? tr(STR_SELECT) : nullptr;
  } else if (currentTab == F1Tab::Drivers) {
    // Tap opens the selected driver's detail; refresh moved to a hold (see
    // loop()), so the hint reflects the tap action instead of STR_F1_REFRESH.
    confirmHint = rows[tab].empty() ? nullptr : tr(STR_F1_DRIVER_DETAILS);
  } else {
    confirmHint = tr(STR_F1_REFRESH);
  }
  // SessionSchedule (static list of times) and the session-results views
  // (scroll with Up/Down only, no more Left/Right cycling between them) have
  // nothing for Left/Right to do.
  const bool showLeftRightHints = (currentTab != F1Tab::SessionSchedule && currentTab != F1Tab::Results &&
                                   currentTab != F1Tab::Qualifying && currentTab != F1Tab::Sprint &&
                                   currentTab != F1Tab::SessionResult);
  const char* leftHint = showLeftRightHints ? tr(STR_DIR_LEFT) : nullptr;
  const char* rightHint = showLeftRightHints ? tr(STR_DIR_RIGHT) : nullptr;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, leftHint, rightHint);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

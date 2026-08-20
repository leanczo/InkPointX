#include "FormulaOneActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdlib>
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

// ISO "YYYY-MM-DD" strings compare correctly with plain string comparison, so
// no date parsing/arithmetic is needed here. Same-day races count as "on or
// before today" (there's no live/pre/post status in this API's schedule
// endpoint to tell a not-yet-started race apart from a finished one on race
// day — confirming one that hasn't actually started yet just surfaces the
// existing "no data" error instead of a result, which is an acceptable edge).
bool isOnOrBeforeToday(const std::string& isoDate) {
  if (isoDate.size() < 10) return false;
  const time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  char todayBuf[11];
  snprintf(todayBuf, sizeof(todayBuf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return isoDate.substr(0, 10) <= std::string(todayBuf);
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

// Ergast/Jolpica's Driver.url is always an English Wikipedia article
// (e.g. "https://en.wikipedia.org/wiki/Lewis_Hamilton"); the REST summary
// endpoint just wants the "Lewis_Hamilton" part, same on every language's
// Wikipedia.
std::string wikipediaTitleFromUrl(const std::string& url) {
  const auto pos = url.find("/wiki/");
  if (pos == std::string::npos) return "";
  return url.substr(pos + 6);
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

  // downloadToFile already writes destPath transactionally (hidden temp file,
  // verified size, atomic replace only on full success — see
  // NetworkFileTransaction in HttpDownloader.cpp) so the previous cache
  // survives untouched if this fails partway through. Downloading straight to
  // cachePath() avoids a second, unchecked manual rename on top of that,
  // which could silently leave stale data in place if it ever failed.
  const auto result = HttpDownloader::downloadToFile(apiUrl(tab), cachePath(tab));
  refreshing[tab] = false;

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
  requestUpdate();
}

bool FormulaOneActivity::loadCacheFromSd(int tab) {
  String input = Storage.readFile(cachePath(tab).c_str());
  if (input.length() == 0) return false;
  parseAndStore(tab, std::string(input.c_str()));
  return loaded[tab];
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
  } else {
    // Calendar: the [0] index below is an ArduinoJson filter wildcard — it
    // applies to every entry in Races[], not just the first one, since we
    // want the whole season's schedule rather than a single race.
    filter["MRData"]["RaceTable"]["Races"][0]["round"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["date"] = true;
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

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("F1", "JSON parse failed for tab %d (%u bytes): %s", tab, static_cast<unsigned>(json.size()), err.c_str());
    errorMessage[tab] = tr(STR_F1_NO_DATA);
    return;
  }

  std::vector<F1Row> newRows;
  std::vector<int> newCalendarRounds;               // only populated for tab == Calendar
  std::vector<std::string> newCalendarDatesIso;      // same, parallel to newCalendarRounds
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
      newCalendarDatesIso.push_back(date);

      F1RaceWeekend wk;
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
    errorMessage[tab] = tr(STR_F1_NO_DATA);
    return;
  }

  rows[tab] = std::move(newRows);
  loaded[tab] = true;
  errorMessage[tab].clear();
  if (tab == static_cast<int>(F1Tab::Drivers)) {
    driverBios = std::move(newDriverBios);
  } else if (tab == static_cast<int>(F1Tab::Calendar)) {
    calendarRounds = std::move(newCalendarRounds);
    calendarDatesIso = std::move(newCalendarDatesIso);
    calendarWeekends = std::move(newCalendarWeekends);
    positionCalendarCursorOnLastPastRace();
  }
}

// One-shot: the first time the Calendar loads, jump its cursor to the most
// recently completed race (found by comparing each row's date to today, via
// the device's own clock — no separate "last results" fetch needed, so this
// works even though Results is no longer a directly reachable tab).
void FormulaOneActivity::positionCalendarCursorOnLastPastRace() {
  if (calendarCursorPositioned) return;

  int idx = -1;
  for (size_t i = 0; i < calendarDatesIso.size(); i++) {
    if (isOnOrBeforeToday(calendarDatesIso[i])) idx = static_cast<int>(i);
  }
  if (idx < 0) return;  // no past race yet (clock not synced, or season hasn't started) — try again next parse

  selectedRow[static_cast<int>(F1Tab::Calendar)] = idx;
  calendarCursorPositioned = true;
}

std::vector<F1Tab> FormulaOneActivity::detailSessionTabs() const {
  std::vector<F1Tab> tabs{F1Tab::Results, F1Tab::Qualifying};
  if (selectedRoundHasSprint) tabs.push_back(F1Tab::Sprint);
  return tabs;
}

void FormulaOneActivity::openDriverDetail(int index) {
  const auto& driverRows = rows[static_cast<int>(F1Tab::Drivers)];
  if (index < 0 || index >= static_cast<int>(driverRows.size()) || index >= static_cast<int>(driverBios.size())) {
    return;
  }
  detailDriverIndex = index;
  showingDriverDetail = true;
  driverBioSummary.clear();  // discard whatever the previously-viewed driver's summary was
  startBioSummaryFetch();
}

void FormulaOneActivity::startBioSummaryFetch() {
  bioSummaryLoading = true;
  bioSummaryFetchFailed = false;
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this](const ActivityResult& result) {
                              if (result.isCancelled) {
                                bioSummaryLoading = false;
                                bioSummaryFetchFailed = true;
                                requestUpdate();
                              } else {
                                doFetchBioSummary();
                              }
                            });
    return;
  }

  doFetchBioSummary();
}

// Wikipedia's REST summary endpoint returns a small, clean JSON document with
// a plain-text "extract" field — no wikitext/HTML to strip, unlike
// CarteleraActivity's synopsis scrape. Spanish is tried first per the user's
// language; some articles (disambiguated English titles especially, e.g.
// ".../George_Russell_(racing_driver)") don't exist under that exact title on
// es.wikipedia.org, so English is the fallback rather than a second guess at
// the Spanish title.
void FormulaOneActivity::doFetchBioSummary() {
  if (detailDriverIndex < 0 || detailDriverIndex >= static_cast<int>(driverBios.size())) return;
  requestUpdateAndWait();  // paint the "Loading bio..." state before the blocking calls below
  wifiWasUsed = true;

  const std::string title = wikipediaTitleFromUrl(driverBios[detailDriverIndex].wikiUrl);
  if (!title.empty()) {
    for (const char* lang : {"es", "en"}) {
      std::string content;
      if (!HttpDownloader::fetchUrl("https://" + std::string(lang) + ".wikipedia.org/api/rest_v1/page/summary/" + title,
                                    content)) {
        continue;
      }
      JsonDocument filter;
      filter["extract"] = true;
      JsonDocument doc;
      if (deserializeJson(doc, content, DeserializationOption::Filter(filter))) continue;
      std::string extract = doc["extract"] | "";
      if (!extract.empty()) {
        bioSummaryLoading = false;
        driverBioSummary = std::move(extract);
        bioSummaryFetchFailed = false;
        requestUpdate();
        return;
      }
    }
  }

  bioSummaryLoading = false;
  bioSummaryFetchFailed = true;
  requestUpdate();
}

// Chronological order for a normal weekend: FP1, FP2, FP3, Qualifying. On a
// sprint weekend FP2/FP3 are simply absent (addSession skips empty dates),
// so the same call order naturally comes out as FP1, Sprint Qualifying,
// Sprint, Qualifying — which is also the real chronological order there.
void FormulaOneActivity::showSessionSchedule(int calendarIdx) {
  const auto& wk = calendarWeekends[calendarIdx];
  std::vector<F1Row> sessionRows;

  auto addSession = [&sessionRows](const char* label, const std::string& date, const std::string& time) {
    if (date.empty()) return;
    sessionRows.push_back(F1Row{label, "", formatSessionLocal(date, time)});
  };
  addSession(tr(STR_F1_SESSION_FP1), wk.fp1Date, wk.fp1Time);
  addSession(tr(STR_F1_SESSION_FP2), wk.fp2Date, wk.fp2Time);
  addSession(tr(STR_F1_SESSION_FP3), wk.fp3Date, wk.fp3Time);
  addSession(tr(STR_F1_SESSION_SPRINT_QUAL), wk.sprintQualDate, wk.sprintQualTime);
  addSession(tr(STR_F1_SESSION_SPRINT), wk.sprintDate, wk.sprintTime);
  addSession(tr(STR_F1_SESSION_QUALIFYING), wk.qualDate, wk.qualTime);

  const int scheduleTab = static_cast<int>(F1Tab::SessionSchedule);
  rows[scheduleTab] = std::move(sessionRows);
  loaded[scheduleTab] = true;
  errorMessage[scheduleTab].clear();
  selectedRow[scheduleTab] = 0;

  const int calTab = static_cast<int>(F1Tab::Calendar);
  sessionScheduleRaceName =
      (calendarIdx >= 0 && calendarIdx < static_cast<int>(rows[calTab].size())) ? rows[calTab][calendarIdx].title : "";

  currentTab = F1Tab::SessionSchedule;
}

void FormulaOneActivity::onEnter() {
  Activity::onEnter();

  for (int tab = 0; tab < F1_TAB_COUNT; tab++) {
    // SessionSchedule is never fetched/cached on its own — it's built
    // in-memory from the Calendar's own data (see showSessionSchedule()).
    if (tab == static_cast<int>(F1Tab::SessionSchedule)) continue;
    if (!loaded[tab]) {
      loadCacheFromSd(tab);
    }
  }

  if (!loaded[static_cast<int>(currentTab)]) {
    startFetch(static_cast<int>(currentTab));
  }

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

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Drilled into a past race from the Calendar tab: Back returns to the
    // calendar instead of exiting. Nothing here needs reloading - Results
    // isn't on screen once we're back on Calendar, so there's no reason to
    // fetch anything (that used to eagerly reload "last race" into the
    // shared raceName/raceDate members, which then stuck around and got
    // shown as a stale subheader the next time a *different* round's fetch
    // was slow or failed - looking like results were always for one fixed
    // race no matter which one got picked).
    if ((currentTab == F1Tab::Results || currentTab == F1Tab::Qualifying || currentTab == F1Tab::Sprint) &&
        selectedRound >= 0) {
      selectedRound = -1;
      currentTab = F1Tab::Calendar;
      requestUpdate();
      return;
    }
    // Session schedule is also reached only from Calendar — same "back to
    // parent" treatment, just without anything to cancel/reload since
    // showing it never involved a fetch in the first place.
    if (currentTab == F1Tab::SessionSchedule) {
      currentTab = F1Tab::Calendar;
      requestUpdate();
      return;
    }
    onGoHome(HomeMenuItem::APPS_MENU);
    return;
  }

  // Results/Qualifying/Sprint/SessionSchedule aren't real tabs — they're only
  // reached by drilling into a Calendar row — so Left/Right on the main bar
  // only cycle the 3 tabs actually in it. Within Results/Qualifying/Sprint,
  // Left/Right instead cycle the sessions available for that weekend (see
  // below); SessionSchedule has nothing to cycle since it's just a static
  // list of times.
  const bool inDetailView = (currentTab == F1Tab::Results || currentTab == F1Tab::Qualifying ||
                              currentTab == F1Tab::Sprint || currentTab == F1Tab::SessionSchedule);
  const bool inSessionResultsView =
      (currentTab == F1Tab::Results || currentTab == F1Tab::Qualifying || currentTab == F1Tab::Sprint);
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && !inDetailView) {
    if (currentTab == F1Tab::Drivers) {
      currentTab = F1Tab::Calendar;
    } else if (currentTab == F1Tab::Constructors) {
      currentTab = F1Tab::Drivers;
    } else {
      currentTab = F1Tab::Constructors;  // was Calendar
    }
    int tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !inDetailView) {
    if (currentTab == F1Tab::Drivers) {
      currentTab = F1Tab::Constructors;
    } else if (currentTab == F1Tab::Constructors) {
      currentTab = F1Tab::Calendar;
    } else {
      currentTab = F1Tab::Drivers;  // was Calendar
    }
    int tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) && inSessionResultsView) {
    const auto sessions = detailSessionTabs();
    const auto it = std::find(sessions.begin(), sessions.end(), currentTab);
    const size_t i = it - sessions.begin();
    currentTab = sessions[(i + sessions.size() - 1) % sessions.size()];
    const int tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty() && !loadCacheFromSd(tab)) {
      startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) && inSessionResultsView) {
    const auto sessions = detailSessionTabs();
    const auto it = std::find(sessions.begin(), sessions.end(), currentTab);
    const size_t i = it - sessions.begin();
    currentTab = sessions[(i + 1) % sessions.size()];
    const int tab = static_cast<int>(currentTab);
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
      const int idx = selectedRow[static_cast<int>(F1Tab::Calendar)];
      if (idx >= 0 && idx < static_cast<int>(calendarRounds.size())) {
        const int round = calendarRounds[idx];
        // Already run: drill into its results (fetched on demand). Hasn't
        // happened yet: show its session schedule instead — already sitting
        // in memory from the Calendar's own fetch, nothing to load.
        if (isOnOrBeforeToday(calendarDatesIso[idx])) {
          selectedRound = round;
          currentTab = F1Tab::Results;
          selectedRoundHasSprint =
              idx < static_cast<int>(calendarWeekends.size()) && !calendarWeekends[idx].sprintDate.empty();
          // Qualifying/Sprint aren't fetched here — only Results, the default
          // landing session, loads eagerly. The other two load lazily the
          // first time the user cycles to them (see Left/Right below), same
          // "only while actively viewing that screen" rule as every other F1
          // fetch in this activity.
          for (F1Tab detailTab : {F1Tab::Results, F1Tab::Qualifying, F1Tab::Sprint}) {
            const int t = static_cast<int>(detailTab);
            loaded[t] = false;
            rows[t].clear();
            errorMessage[t].clear();
          }
          // raceName/raceDate are shared (not per-tab) subheader state, so
          // clear them along with rows[resultsTab] - otherwise a failed or
          // slow fetch for this round would leave the previous round's name
          // showing above an empty/error body.
          raceName.clear();
          raceDate.clear();
          const int resultsTab = static_cast<int>(F1Tab::Results);
          if (!loadCacheFromSd(resultsTab)) {
            startFetch(resultsTab);
          }
          requestUpdate();
        } else if (idx < static_cast<int>(calendarWeekends.size())) {
          showSessionSchedule(idx);
          requestUpdate();
        }
      }
    } else if (currentTab == F1Tab::Drivers) {
      openDriverDetail(selectedRow[static_cast<int>(F1Tab::Drivers)]);
    } else if (currentTab != F1Tab::SessionSchedule) {
      // SessionSchedule has nothing to fetch — its data already lives in
      // memory from the Calendar's own fetch (see showSessionSchedule()).
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

    int textY = contentTop + metrics.subHeaderHeight + metrics.verticalSpacing;
    const int textLeft = metrics.contentSidePadding;
    const int wrapWidth = pageWidth - 2 * metrics.contentSidePadding;
    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int infoLineHeight = renderer.getLineHeight(UI_10_FONT_ID);

    // Number/code/nationality/DOB/points as one compact line — bio.number and
    // bio.code can each independently be empty (some historical/reserve
    // entries lack a permanent number or three-letter code).
    std::string infoLine;
    if (!bio.number.empty()) infoLine += "#" + bio.number + " ";
    if (!bio.code.empty()) infoLine += bio.code + " · ";
    if (!bio.nationality.empty()) infoLine += bio.nationality + " · ";
    if (!bio.dateOfBirth.empty()) infoLine += fullDate(bio.dateOfBirth) + " · ";
    infoLine += driverRow.value + " pts";
    renderer.drawText(UI_10_FONT_ID, textLeft, textY, infoLine.c_str(), true);
    textY += infoLineHeight + metrics.verticalSpacing;

    if (bioSummaryLoading) {
      const int y = textY + (listBottom - textY) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_F1_BIO_LOADING));
    } else if (bioSummaryFetchFailed || driverBioSummary.empty()) {
      const int y = textY + (listBottom - textY) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_F1_BIO_UNAVAILABLE));
    } else {
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int maxLines = std::max(1, (listBottom - textY) / lineHeight);
      auto lines = renderer.wrappedText(UI_10_FONT_ID, driverBioSummary.c_str(), wrapWidth, maxLines, EpdFontFamily::REGULAR);
      for (const auto& line : lines) {
        renderer.drawText(UI_10_FONT_ID, textLeft, textY, line.c_str(), true, EpdFontFamily::REGULAR);
        textY += lineHeight;
      }
    }

    const char* qrHint = bio.wikiUrl.empty() ? nullptr : tr(STR_F1_SHOW_QR);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), qrHint, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Results/Qualifying/Sprint/SessionSchedule aren't in the main tab bar —
  // they're detail views reached by drilling into a Calendar row.
  // SessionSchedule keeps the main bar frozen on Calendar (its parent);
  // Results/Qualifying/Sprint instead swap it for a mini strip of the
  // sessions available for that weekend, since Left/Right cycle between
  // those there (see loop()).
  const bool inSessionResultsView =
      (currentTab == F1Tab::Results || currentTab == F1Tab::Qualifying || currentTab == F1Tab::Sprint);
  const int tabBarY = metrics.topPadding + metrics.headerHeight + 20;
  if (inSessionResultsView) {
    const auto sessions = detailSessionTabs();
    std::vector<std::string> sessionLabels;
    int selectedSessionIndex = 0;
    for (size_t i = 0; i < sessions.size(); i++) {
      if (sessions[i] == currentTab) selectedSessionIndex = static_cast<int>(i);
      // tr() textually substitutes "StrId::" in front of its argument, so it
      // can't take a ternary directly (that would only prefix the first
      // operand) — resolve the StrId first, then look it up.
      const StrId labelId = sessions[i] == F1Tab::Qualifying ? StrId::STR_F1_SESSION_QUALIFYING
                            : sessions[i] == F1Tab::Sprint    ? StrId::STR_F1_SESSION_SPRINT
                                                               : StrId::STR_F1_SESSION_RACE;
      sessionLabels.push_back(I18n::getInstance().get(labelId));
    }
    drawTabStrip(tabBarY, sessionLabels, selectedSessionIndex);
  } else {
    const std::vector<std::string> tabLabels = {tr(STR_F1_TAB_DRIVERS), tr(STR_F1_TAB_CONSTRUCTORS),
                                                 tr(STR_F1_TAB_CALENDAR)};
    int selectedTabIndex = 2;  // Calendar
    if (currentTab == F1Tab::Drivers) selectedTabIndex = 0;
    else if (currentTab == F1Tab::Constructors) selectedTabIndex = 1;
    drawTabStrip(tabBarY, tabLabels, selectedTabIndex);
  }

  int contentTop = tabBarY + 30 + metrics.verticalSpacing;

  if (inSessionResultsView && !raceName.empty()) {
    const std::string displayDate = shortDate(raceDate);
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight}, raceName.c_str(),
                       displayDate.c_str());
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
    const bool isCalendar = (currentTab == F1Tab::Calendar);
    // Results/Qualifying/Sprint skip the team subtitle so rows use the
    // shorter row height and more drivers fit on screen at once; team name
    // isn't essential there since it's already shown in the Drivers
    // standings tab.
    std::function<std::string(int)> subtitleFn;
    if (!inSessionResultsView) {
      subtitleFn = [&tabRows](int i) { return tabRows[i].subtitle; };
    }
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, listBottom - contentTop}, static_cast<int>(tabRows.size()),
        selectedRow[tab], [&tabRows](int i) { return tabRows[i].title; }, subtitleFn, nullptr,
        [this, &tabRows, isCalendar](int i) {
          // Checked against today's date live (not baked in at parse time) so
          // a race happening today doesn't need the whole calendar list
          // re-fetched to become viewable once it's actually done.
          if (isCalendar && i < static_cast<int>(calendarDatesIso.size()) && isOnOrBeforeToday(calendarDatesIso[i])) {
            return std::string(tr(STR_F1_VIEW_RESULT));
          }
          return tabRows[i].value;
        },
        true);
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
    confirmHint = nullptr;  // static data already sitting in memory — nothing to refresh
  } else if (currentTab == F1Tab::Drivers) {
    // Tap opens the selected driver's detail; refresh moved to a hold (see
    // loop()), so the hint reflects the tap action instead of STR_F1_REFRESH.
    confirmHint = rows[tab].empty() ? nullptr : tr(STR_F1_DRIVER_DETAILS);
  } else {
    confirmHint = tr(STR_F1_REFRESH);
  }
  // SessionSchedule has nothing to cycle (static list of times); every other
  // tab, including the session-results detail views, has a working Left/Right.
  const bool showLeftRightHints = (currentTab != F1Tab::SessionSchedule);
  const char* leftHint = showLeftRightHints ? tr(STR_DIR_LEFT) : nullptr;
  const char* rightHint = showLeftRightHints ? tr(STR_DIR_RIGHT) : nullptr;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, leftHint, rightHint);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

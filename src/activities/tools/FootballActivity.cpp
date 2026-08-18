#include "FootballActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

namespace {

struct AvailableLeague {
  const char* slug;
  const char* name;
};

// Curated so users pick from a list instead of typing a raw ESPN slug (this
// device has no physical keyboard). Slugs verified against ESPN's public,
// keyless soccer scoreboard/standings endpoints.
constexpr AvailableLeague AVAILABLE_LEAGUES[] = {
    {"arg.1", "Liga Profesional Argentina"},
    {"conmebol.libertadores", "Copa Libertadores"},
    {"conmebol.sudamericana", "Copa Sudamericana"},
    {"eng.1", "Premier League"},
    {"esp.1", "La Liga"},
    {"ita.1", "Serie A"},
    {"ger.1", "Bundesliga"},
    {"fra.1", "Ligue 1"},
    {"uefa.champions", "UEFA Champions League"},
    {"uefa.europa", "UEFA Europa League"},
    {"bra.1", "Brasileir\xC3\xA3o"},
    {"mex.1", "Liga MX"},
    {"usa.1", "MLS"},
};
constexpr int NUM_AVAILABLE_LEAGUES = sizeof(AVAILABLE_LEAGUES) / sizeof(AVAILABLE_LEAGUES[0]);

// Matches HttpDownloader's own MIN_TLS_LARGEST_BLOCK: below this, TLS
// handshakes on this PSRAM-less C3 start failing outright. onExit() already
// clears fragmentation via silentRestart() when the user leaves, but a long
// session of day-by-day browsing without leaving can cross this line first -
// check proactively after every fetch instead of waiting for that exit.
constexpr size_t kCriticalLargestFreeBlock = 32 * 1024;

std::string sanitizeSlug(const std::string& slug) {
  std::string s = slug;
  for (char& c : s) {
    if (c == '.') c = '_';
  }
  return s;
}

struct LocalEventTime {
  int month, day, hour, minute;
  bool valid;
};

// ESPN dates are ISO-8601 UTC, e.g. "2026-08-01T18:30Z". Shifts to the same
// UTC offset the Clock app uses (SETTINGS.clockUtcOffsetQ, in quarter-hours)
// and rolls the calendar fields by hand rather than mktime/timegm, which
// would pull in libc's unconfigured timezone state on this target.
LocalEventTime toLocalEventTime(const std::string& iso) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) {
    return LocalEventTime{0, 0, 0, 0, false};
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

  return LocalEventTime{mo, d, totalMinutes / 60, totalMinutes % 60, true};
}

std::string formatEventTimeOnly(const std::string& iso) {
  const LocalEventTime t = toLocalEventTime(iso);
  if (!t.valid) return "";
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
  return buf;
}

std::string formatEventDateOnly(const std::string& iso) {
  const LocalEventTime t = toLocalEventTime(iso);
  if (!t.valid) return "";
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d/%02d", t.month, t.day);
  return buf;
}

// ESPN's status.type.description field comes straight from the API and is
// always in English regardless of the device's language setting. Translate
// the fixed set of values it's actually observed to send; anything
// unrecognized falls back to ESPN's raw text instead of showing nothing.
std::string translateMatchStatus(const std::string& desc) {
  if (desc == "Scheduled") return tr(STR_FOOTBALL_STATUS_SCHEDULED);
  if (desc == "Full Time") return tr(STR_FOOTBALL_STATUS_FULL_TIME);
  if (desc == "Halftime") return tr(STR_FOOTBALL_STATUS_HALFTIME);
  if (desc == "In Progress") return tr(STR_FOOTBALL_STATUS_IN_PROGRESS);
  if (desc == "Postponed") return tr(STR_FOOTBALL_STATUS_POSTPONED);
  if (desc == "Canceled" || desc == "Cancelled") return tr(STR_FOOTBALL_STATUS_CANCELED);
  return desc;
}

// Scans `file` from its current position for every "rank"/"points" stat
// value, appending each to the matching output vector in the order
// encountered (the same order ArduinoJson's own entries iteration sees,
// since both just walk the file front to back). Reads in fixed-size chunks
// and keeps only a small trailing window, so memory use stays flat no
// matter how many teams the file lists - unlike filtering the "stats" array
// through ArduinoJson, which would keep all ~14 categories for every team
// just to get at these two.
//
// Matches on the literal bytes ESPN sends, e.g.:
//   {"name":"rank",...,"displayValue":"10"}
//   {"name":"points",...,"displayValue":"6"}
// The trailing `"` in each needle is what keeps "rank" from matching inside
// "rankChange", and "points" from matching inside "pointsFor"/"pointsAgainst"
// (verified against live responses for two unrelated leagues).
void extractRankAndPoints(HalFile& file, std::vector<int>& ranks, std::vector<int>& points) {
  static constexpr char kRankNeedle[] = "\"name\":\"rank\"";
  static constexpr char kPointsNeedle[] = "\"name\":\"points\"";
  static constexpr char kValueNeedle[] = "\"displayValue\":\"";
  constexpr size_t kChunkSize = 1024;
  // Comfortably longer than any needle plus the digits that follow it, so a
  // match split across a chunk boundary is never lost when trimming below.
  constexpr size_t kKeepMargin = 48;

  std::string window;
  bool eof = false;
  size_t pos = 0;

  auto refill = [&]() {
    if (eof) return false;
    char chunk[kChunkSize];
    const int n = file.read(chunk, kChunkSize);
    if (n <= 0) {
      eof = true;
      return false;
    }
    window.append(chunk, static_cast<size_t>(n));
    return true;
  };

  auto valueAfter = [&](size_t fromPos) -> int {
    for (;;) {
      const size_t valuePos = window.find(kValueNeedle, fromPos);
      if (valuePos == std::string::npos) {
        if (!refill()) return 0;
        continue;
      }
      const size_t digitsStart = valuePos + sizeof(kValueNeedle) - 1;
      const size_t digitsEnd = window.find('"', digitsStart);
      if (digitsEnd == std::string::npos) {
        if (!refill()) return 0;
        continue;
      }
      return atoi(window.substr(digitsStart, digitsEnd - digitsStart).c_str());
    }
  };

  for (;;) {
    const size_t rankPos = window.find(kRankNeedle, pos);
    const size_t pointsPos = window.find(kPointsNeedle, pos);
    if (rankPos == std::string::npos && pointsPos == std::string::npos) {
      if (!refill()) break;
      continue;
    }
    if (pointsPos != std::string::npos && (rankPos == std::string::npos || pointsPos < rankPos)) {
      points.push_back(valueAfter(pointsPos));
      pos = pointsPos + sizeof(kPointsNeedle) - 1;
    } else {
      ranks.push_back(valueAfter(rankPos));
      pos = rankPos + sizeof(kRankNeedle) - 1;
    }

    if (pos > kKeepMargin) {
      window.erase(0, pos - kKeepMargin);
      pos = kKeepMargin;
    }
  }
}

}  // namespace

std::string FootballActivity::cachePath(int tab) const {
  const std::string slug = sanitizeSlug(subscriptions[activeLeagueIndex].slug);
  if (tab == static_cast<int>(FootballTab::Standings)) return "/apps/football/" + slug + "_standings.json";
  return "/apps/football/" + slug + "_results_" + resultsDateYYYYMMDD() + ".json";
}

std::string FootballActivity::tmpPath(int tab) const {
  const std::string slug = sanitizeSlug(subscriptions[activeLeagueIndex].slug);
  if (tab == static_cast<int>(FootballTab::Standings)) return "/apps/football/" + slug + "_standings.tmp.json";
  return "/apps/football/" + slug + "_results_" + resultsDateYYYYMMDD() + ".tmp.json";
}

std::string FootballActivity::resultsDateYYYYMMDD() const {
  const time_t target = time(nullptr) - static_cast<time_t>(resultsDayOffset) * 86400;
  struct tm t;
  gmtime_r(&target, &t);
  // GCC can't see gmtime_r's actual output range, so -Wformat-truncation
  // assumes each field could be a signed 11-digit extreme; the modulo gives
  // it a provable bound (year: 4 digits + sign, month/day: 2 + sign) instead
  // of growing the buffer to match the unbounded worst case.
  const int year = (t.tm_year + 1900) % 10000;
  const int month = (t.tm_mon + 1) % 100;
  const int day = t.tm_mday % 100;
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d%02d%02d", year, month, day);
  return buf;
}

std::string FootballActivity::apiUrl(int tab) const {
  const std::string& slug = subscriptions[activeLeagueIndex].slug;
  // site.web.api.espn.com serves the identical site-API JSON (same paths,
  // same payload) but isn't behind the Akamai WAF guarding site.api.espn.com,
  // which 403s a non-browser client regardless of User-Agent - verified live
  // against both endpoints, repeatedly, with the device's own UA.
  if (tab == static_cast<int>(FootballTab::Standings)) {
    return "https://site.web.api.espn.com/apis/v2/sports/soccer/" + slug + "/standings";
  }
  // Single-day query only: a multi-day window's filtered JsonDocument still
  // retains every event in range (the ArduinoJson Filter's [0] wildcard
  // applies to every array element, not just the first), which was enough to
  // exhaust heap on busy leagues/rounds. Older matches are reached by
  // stepping resultsDayOffset (see changeResultsDay) instead of widening the
  // window.
  return "https://site.web.api.espn.com/apis/site/v2/sports/soccer/" + slug +
         "/scoreboard?dates=" + resultsDateYYYYMMDD();
}

void FootballActivity::loadSubscriptions() {
  subscriptions.clear();
  String content = Storage.readFile("/apps/football/leagues.txt");
  std::string data(content.c_str());

  size_t pos = 0;
  while (pos < data.length()) {
    size_t nl = data.find('\n', pos);
    std::string line = (nl == std::string::npos) ? data.substr(pos) : data.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t sep = line.find('|');
    if (!line.empty() && sep != std::string::npos) {
      subscriptions.push_back(FootballLeague{line.substr(0, sep), line.substr(sep + 1)});
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }

  if (subscriptions.empty()) {
    subscriptions.push_back(FootballLeague{"arg.1", "Liga Profesional Argentina"});
    saveSubscriptions();
  }
}

void FootballActivity::saveSubscriptions() {
  String content;
  for (const auto& league : subscriptions) {
    content += String(league.slug.c_str()) + "|" + String(league.displayName.c_str()) + "\n";
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/football");
  Storage.writeFile("/apps/football/leagues.txt", content);
}

void FootballActivity::rebuildAddLeagueList() {
  addLeagueAvailableIndices.clear();
  for (int i = 0; i < NUM_AVAILABLE_LEAGUES; i++) {
    bool alreadySubscribed = false;
    for (const auto& league : subscriptions) {
      if (league.slug == AVAILABLE_LEAGUES[i].slug) {
        alreadySubscribed = true;
        break;
      }
    }
    if (!alreadySubscribed) {
      addLeagueAvailableIndices.push_back(i);
    }
  }
  if (selectedAddIndex >= static_cast<int>(addLeagueAvailableIndices.size())) {
    selectedAddIndex = 0;
  }
}

void FootballActivity::startFetch(int tab) {
  refreshing[tab] = true;
  refreshFailed[tab] = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/football");

  // Free both tabs' vectors before the TLS handshake: mbedTLS needs a
  // contiguous ~32KB (16KB in + 16KB out) buffer on this PSRAM-less chip, and
  // a resident results/standings list is enough to fragment that away. The
  // active tab is reloaded from the (untouched-on-failure) SD cache once the
  // fetch completes; the other, currently off-screen tab just gets marked
  // unloaded so it lazily reloads from its own cache next time it's viewed.
  const int otherTab = 1 - tab;
  if (otherTab == static_cast<int>(FootballTab::Results)) {
    resultsMatches.clear();
    resultsMatches.shrink_to_fit();
  } else {
    standingsGroups.clear();
    standingsGroups.shrink_to_fit();
  }
  loaded[otherTab] = false;
  errorMessage[otherTab].clear();

  if (tab == static_cast<int>(FootballTab::Results)) {
    resultsMatches.clear();
    resultsMatches.shrink_to_fit();
  } else {
    standingsGroups.clear();
    standingsGroups.shrink_to_fit();
  }

  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this, tab](const ActivityResult& result) {
                              if (result.isCancelled) {
                                refreshing[tab] = false;
                                // startFetch() already cleared this tab's vector above;
                                // restore it from disk since the fetch never started.
                                loadCacheFromSd(tab);
                                if (!loaded[tab]) {
                                  errorMessage[tab] = tr(STR_FOOTBALL_WIFI_REQUIRED);
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

void FootballActivity::doFetch(int tab) {
  requestUpdateAndWait();  // paint the "Refreshing..." state before the blocking call below
  wifiWasUsed = true;

  LOG_DBG("FOOTBALL", "Pre-fetch tab %d: heap free=%u largest=%u", tab, (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  const auto result = HttpDownloader::downloadToFile(apiUrl(tab), tmpPath(tab));
  LOG_DBG("FOOTBALL", "Post-fetch tab %d: result=%d heap free=%u largest=%u", tab, static_cast<int>(result),
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  refreshing[tab] = false;

  if (result == HttpDownloader::OK) {
    Storage.remove(cachePath(tab).c_str());
    Storage.rename(tmpPath(tab).c_str(), cachePath(tab).c_str());
  } else {
    // getLastLogs() grows a std::string via repeated append() calls (up to
    // ~4KB across its 16 ring-buffer lines); under -fno-exceptions a failed
    // grow calls abort() instead of throwing, and that fired here in the
    // field. A bounded, stack-only line is the safe way to persist this -
    // read /apps/football/debug.log from the SD card (no USB needed).
    char debugLine[128];
    const int len = snprintf(debugLine, sizeof(debugLine), "tab=%d result=%d heap free=%u largest=%u\n", tab,
                              static_cast<int>(result), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (len > 0) {
      HalFile debugFile = Storage.open("/apps/football/debug.log", O_WRITE | O_CREAT | O_APPEND);
      if (debugFile) {
        debugFile.write(debugLine, static_cast<size_t>(std::min(len, static_cast<int>(sizeof(debugLine)) - 1)));
        debugFile.close();
      }
    }
  }

  // The vector for this tab was cleared before the fetch started, so the
  // reload must happen unconditionally — on failure this is the only way to
  // get the old (still-good, untouched-on-disk) data back.
  loadCacheFromSd(tab);
  if (!loaded[tab]) {
    if (errorMessage[tab].empty()) errorMessage[tab] = tr(STR_FOOTBALL_NO_DATA);
  } else if (result != HttpDownloader::OK) {
    refreshFailed[tab] = true;
  }
  requestUpdate();

  // silentRestart() never returns (ESP.restart()); nothing below it in this
  // call runs, which is fine - the cache rename/reload above already landed
  // on disk, and it reboots straight back to Home (see SilentRestart.h).
  if (ESP.getMaxAllocHeap() < kCriticalLargestFreeBlock) {
    LOG_INF("FOOTBALL", "Largest free block %u below %u, clearing fragmentation early",
            (unsigned)ESP.getMaxAllocHeap(), (unsigned)kCriticalLargestFreeBlock);
    silentRestart();
  }
}

namespace {
// Bounds how far back a user can page. Beyond this, cached per-day result
// files would accumulate indefinitely and the browsing is no longer useful
// for a live-scores app anyway.
constexpr int kMaxResultsDayOffset = 14;
}  // namespace

void FootballActivity::changeResultsDay(int delta) {
  const int newOffset = std::clamp(resultsDayOffset + delta, 0, kMaxResultsDayOffset);
  if (newOffset == resultsDayOffset) return;
  resultsDayOffset = newOffset;

  const int tab = static_cast<int>(FootballTab::Results);
  loaded[tab] = false;
  errorMessage[tab].clear();
  resultsMatches.clear();
  resultsMatches.shrink_to_fit();
  selectedResultsRow = 0;

  if (!loadCacheFromSd(tab)) startFetch(tab);
  requestUpdate();
}

bool FootballActivity::loadCacheFromSd(int tab) {
  HalFile file;
  if (!Storage.openFileForRead("FOOTBALL", cachePath(tab).c_str(), file)) {
    return false;
  }
  parseAndStore(tab, file);
  return loaded[tab];
}

void FootballActivity::parseAndStore(int tab, HalFile& file) {
  // Feeding the open file straight to ArduinoJson lets it scan the raw bytes
  // a chunk at a time; only the small *filtered* result tree is kept, rather
  // than holding the whole response in RAM first.
  struct HalFileJsonReader {
    HalFile& f;
    int read() { return f.read(); }
    size_t readBytes(char* buffer, size_t length) {
      const int n = f.read(buffer, length);
      return n < 0 ? 0 : static_cast<size_t>(n);
    }
  } reader{file};

  JsonDocument filter;
  if (tab == static_cast<int>(FootballTab::Results)) {
    filter["events"][0]["date"] = true;
    filter["events"][0]["status"]["type"]["state"] = true;
    filter["events"][0]["status"]["type"]["description"] = true;
    filter["events"][0]["competitions"][0]["competitors"][0]["homeAway"] = true;
    filter["events"][0]["competitions"][0]["competitors"][0]["score"] = true;
    filter["events"][0]["competitions"][0]["competitors"][0]["team"]["displayName"] = true;
  } else {
    filter["children"][0]["name"] = true;
    filter["children"][0]["standings"]["entries"][0]["team"]["displayName"] = true;
    // "stats" deliberately excluded: ArduinoJson's Filter can only keep a
    // field on every element of an array or on none of them (it always
    // checks filter index 0 and reuses that one decision for every element -
    // see parseArray() in ArduinoJson/Json/JsonDeserializer.hpp), and each
    // entry's stats array has ~14 categories. Asking for just "rank" and
    // "points" here would still retain all 14 x every team. Those two values
    // are pulled separately via extractRankAndPoints(), which scans the raw
    // file text instead of materializing the JSON tree for it.
  }

  JsonDocument doc;
  // ESPN's scoreboard/standings responses nest well past ArduinoJson's default
  // limit of 10 (broadcast/venue/link metadata we filter out still counts
  // toward nesting while being scanned) - raised, or every parse fails with
  // "TooDeep" regardless of how small the filtered-down result is.
  LOG_DBG("FOOTBALL", "Pre-parse tab %d: heap free=%u largest=%u", tab, (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  DeserializationError err =
      deserializeJson(doc, reader, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(20));
  LOG_DBG("FOOTBALL", "Post-parse tab %d: heap free=%u largest=%u", tab, (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  if (err) {
    LOG_ERR("FOOTBALL", "JSON parse failed for tab %d: %s, heap free=%u largest=%u", tab, err.c_str(),
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    errorMessage[tab] = tr(STR_FOOTBALL_NO_DATA);
    return;
  }

  std::vector<FootballMatch> newMatches;
  std::vector<FootballGroup> newGroups;

  if (tab == static_cast<int>(FootballTab::Results)) {
    constexpr size_t MAX_RESULT_ROWS = 20;  // defensive cap regardless of window size
    JsonArray events = doc["events"];
    newMatches.reserve(std::min(events.size(), MAX_RESULT_ROWS));
    for (JsonObject event : events) {
      if (newMatches.size() >= MAX_RESULT_ROWS) break;
      std::string state = event["status"]["type"]["state"] | "";
      std::string desc = event["status"]["type"]["description"] | "";
      std::string dateStr = event["date"] | "";

      std::string home, away, homeScore, awayScore;
      JsonArray competitors = event["competitions"][0]["competitors"];
      for (JsonObject c : competitors) {
        std::string homeAway = c["homeAway"] | "";
        std::string name = c["team"]["displayName"] | "";
        std::string score = c["score"] | "";
        if (homeAway == "home") {
          home = name;
          homeScore = score;
        } else {
          away = name;
          awayScore = score;
        }
      }

      newMatches.push_back(FootballMatch{home, away, homeScore, awayScore, state, desc, dateStr});
    }
    // Most recently played first: events already arrive in chronological
    // order from ESPN, so just reverse rather than re-sorting.
    std::reverse(newMatches.begin(), newMatches.end());
  } else {
    // rank/points for every team come from extractRankAndPoints() scanning
    // the raw file (see its comment above for why), in the same left-to-
    // right order this loop visits entries in - so team #i's values are
    // simply ranks[i]/points[i] read off a running counter below.
    std::vector<int> ranks, points;
    file.seek(0);
    extractRankAndPoints(file, ranks, points);
    size_t statIndex = 0;

    // ESPN doesn't return entries pre-sorted by rank, so sort each group
    // (zone) separately by rank before appending; groups themselves stay in
    // ESPN's own order and are kept as separate FootballGroups so the UI can
    // show one group's table at a time instead of one long merged list.
    struct StandingRow {
      int rank;
      std::string team;
      int points;
    };

    JsonArray children = doc["children"];
    newGroups.reserve(children.size());
    for (JsonObject child : children) {
      std::string groupName = child["name"] | "";
      JsonArray entries = child["standings"]["entries"];

      std::vector<StandingRow> groupRows;
      groupRows.reserve(entries.size());
      for (JsonObject entry : entries) {
        std::string team = entry["team"]["displayName"] | "";
        const int rank = statIndex < ranks.size() ? ranks[statIndex] : 0;
        const int pts = statIndex < points.size() ? points[statIndex] : 0;
        statIndex++;
        groupRows.push_back(StandingRow{rank, team, pts});
      }
      std::sort(groupRows.begin(), groupRows.end(),
                [](const StandingRow& a, const StandingRow& b) { return a.rank < b.rank; });

      FootballGroup group;
      group.name = groupName;
      group.rows.reserve(groupRows.size());
      for (const auto& row : groupRows) {
        group.rows.push_back(
            FootballRow{std::to_string(row.rank) + ". " + row.team, "", std::to_string(row.points) + " pts"});
      }
      newGroups.push_back(std::move(group));
    }

    // If any entry were missing a "rank" or "points" stat, the scan would
    // have found fewer values than there are teams, and every team after the
    // gap would silently show some other team's numbers (statIndex just
    // keeps counting). Bail out to the normal "no data" state instead of
    // risking that - this has held for every league checked so far, but
    // it's cheap to verify on every parse rather than assume it forever.
    if (statIndex != ranks.size() || statIndex != points.size()) {
      LOG_ERR("FOOTBALL", "Standings rank/points count mismatch: %u teams, %u ranks, %u points",
              static_cast<unsigned>(statIndex), static_cast<unsigned>(ranks.size()),
              static_cast<unsigned>(points.size()));
      errorMessage[tab] = tr(STR_FOOTBALL_NO_DATA);
      return;
    }
  }

  const bool isResults = tab == static_cast<int>(FootballTab::Results);
  size_t totalRows = isResults ? newMatches.size() : 0;
  if (!isResults) {
    for (const auto& group : newGroups) totalRows += group.rows.size();
  }

  if (totalRows == 0) {
    if (isResults) {
      // Results only ever queries one day now (see apiUrl), and most
      // leagues don't play every day - zero matches is the normal case, not
      // a failure. Mark the tab loaded (with an empty list) so render()
      // shows "no matches" instead of the generic "couldn't load" error.
      resultsMatches.clear();
      selectedResultsRow = 0;
      loaded[tab] = true;
      errorMessage[tab].clear();
      return;
    }
    LOG_ERR("FOOTBALL", "Parsed JSON for tab %d but found 0 rows (unexpected shape?), heap free=%u largest=%u", tab,
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    errorMessage[tab] = tr(STR_FOOTBALL_NO_DATA);
    return;
  }

  if (isResults) {
    resultsMatches = std::move(newMatches);
    selectedResultsRow = 0;
  } else {
    standingsGroups = std::move(newGroups);
    selectedGroupIndex = 0;
    selectedGroupRow = 0;
  }
  loaded[tab] = true;
  errorMessage[tab].clear();
}

void FootballActivity::onEnter() {
  Activity::onEnter();
  loadSubscriptions();
  rebuildAddLeagueList();
  requestUpdate();
}

void FootballActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void FootballActivity::loop() {
  if (state == FootballState::LeagueSelection) {
    const int totalItems = static_cast<int>(subscriptions.size()) + 1;  // + "Add League"
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome(HomeMenuItem::APPS_MENU);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedSubIndex = (selectedSubIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedSubIndex = (selectedSubIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedSubIndex == totalItems - 1) {
        rebuildAddLeagueList();
        selectedAddIndex = 0;
        state = FootballState::AddLeague;
        requestUpdate();
      } else {
        activeLeagueIndex = selectedSubIndex;
        currentTab = FootballTab::Results;
        state = FootballState::LeagueDetail;
        for (int t = 0; t < 2; t++) {
          loaded[t] = false;
          errorMessage[t].clear();
        }
        resultsMatches.clear();
        selectedResultsRow = 0;
        resultsDayOffset = 0;
        standingsGroups.clear();
        selectedGroupIndex = 0;
        selectedGroupRow = 0;
        if (!loadCacheFromSd(static_cast<int>(currentTab))) {
          startFetch(static_cast<int>(currentTab));
        }
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (selectedSubIndex >= 0 && selectedSubIndex < totalItems - 1) {
        const FootballLeague toDelete = subscriptions[selectedSubIndex];
        auto handler = [this, toDelete](const ActivityResult& res) {
          if (!res.isCancelled) {
            for (size_t i = 0; i < subscriptions.size(); i++) {
              if (subscriptions[i].slug == toDelete.slug) {
                subscriptions.erase(subscriptions.begin() + i);
                break;
              }
            }
            saveSubscriptions();
            selectedSubIndex = 0;
            const std::string slug = sanitizeSlug(toDelete.slug);
            Storage.remove(("/apps/football/" + slug + "_standings.json").c_str());
            // Results are cached per day (see cachePath), so sweep the whole
            // directory for this league's files instead of one fixed name.
            const std::string prefix = slug + "_results_";
            for (const auto& entry : Storage.listFiles("/apps/football")) {
              const std::string name = entry.c_str();
              if (name.rfind(prefix, 0) == 0) {
                Storage.remove(("/apps/football/" + name).c_str());
              }
            }
          }
          requestUpdate();
        };
        startActivityForResult(
            makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_FOOTBALL_UNSUBSCRIBE),
                                                     toDelete.displayName),
            handler);
      }
    }
  } else if (state == FootballState::AddLeague) {
    const int totalAvailable = static_cast<int>(addLeagueAvailableIndices.size());
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = FootballState::LeagueSelection;
      requestUpdate();
    } else if (totalAvailable > 0 && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedAddIndex = (selectedAddIndex - 1 + totalAvailable) % totalAvailable;
      requestUpdate();
    } else if (totalAvailable > 0 && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedAddIndex = (selectedAddIndex + 1) % totalAvailable;
      requestUpdate();
    } else if (totalAvailable > 0 && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const AvailableLeague& picked = AVAILABLE_LEAGUES[addLeagueAvailableIndices[selectedAddIndex]];
      subscriptions.push_back(FootballLeague{picked.slug, picked.name});
      saveSubscriptions();
      selectedSubIndex = static_cast<int>(subscriptions.size()) - 1;
      state = FootballState::LeagueSelection;
      requestUpdate();
    }
  } else if (state == FootballState::LeagueDetail) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = FootballState::LeagueSelection;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      // Within Standings, multiple groups (zones) act as their own tab strip:
      // Left/Right first walks group-by-group, only falling through to the
      // Results<->Standings toggle once you step off either end.
      if (currentTab == FootballTab::Standings && standingsGroups.size() > 1 && selectedGroupIndex > 0) {
        selectedGroupIndex--;
        selectedGroupRow = 0;
      } else {
        currentTab = static_cast<FootballTab>(ButtonNavigator::previousIndex(static_cast<int>(currentTab), 2));
        if (currentTab == FootballTab::Standings && standingsGroups.size() > 1) {
          selectedGroupIndex = static_cast<int>(standingsGroups.size()) - 1;
        }
        selectedGroupRow = 0;
        int tab = static_cast<int>(currentTab);
        if (!loaded[tab] && errorMessage[tab].empty()) {
          if (!loadCacheFromSd(tab)) startFetch(tab);
        }
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (currentTab == FootballTab::Standings && standingsGroups.size() > 1 &&
          selectedGroupIndex < static_cast<int>(standingsGroups.size()) - 1) {
        selectedGroupIndex++;
        selectedGroupRow = 0;
      } else {
        currentTab = static_cast<FootballTab>(ButtonNavigator::nextIndex(static_cast<int>(currentTab), 2));
        if (currentTab == FootballTab::Standings) {
          selectedGroupIndex = 0;
          selectedGroupRow = 0;
        }
        int tab = static_cast<int>(currentTab);
        if (!loaded[tab] && errorMessage[tab].empty()) {
          if (!loadCacheFromSd(tab)) startFetch(tab);
        }
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (currentTab == FootballTab::Standings) {
        if (!standingsGroups.empty()) {
          auto& groupRows = standingsGroups[selectedGroupIndex].rows;
          if (!groupRows.empty()) {
            selectedGroupRow = static_cast<int>((selectedGroupRow - 1 + groupRows.size()) % groupRows.size());
            requestUpdate();
          }
        }
      } else if (!resultsMatches.empty() && selectedResultsRow > 0) {
        selectedResultsRow--;
        requestUpdate();
      } else {
        // Top of the (possibly empty) list: step to the previous day instead
        // of wrapping, same boundary-then-fallthrough shape Left/Right use
        // for standings groups above.
        changeResultsDay(+1);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (currentTab == FootballTab::Standings) {
        if (!standingsGroups.empty()) {
          auto& groupRows = standingsGroups[selectedGroupIndex].rows;
          if (!groupRows.empty()) {
            selectedGroupRow = static_cast<int>((selectedGroupRow + 1) % groupRows.size());
            requestUpdate();
          }
        }
      } else if (!resultsMatches.empty() && selectedResultsRow < static_cast<int>(resultsMatches.size()) - 1) {
        selectedResultsRow++;
        requestUpdate();
      } else {
        changeResultsDay(-1);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startFetch(static_cast<int>(currentTab));
    }
  }
}

// Paginates the same way LyraTheme::drawButtonMenu does (window of pageItems
// rows centered on the selection, clamped to bounds) since GUI.drawList can't
// be reused here — it only draws plain title/subtitle/value text, not a
// custom score box.
void FootballActivity::drawResultsList(int x, int y, int width, int height) {
  const int totalRows = static_cast<int>(resultsMatches.size());
  if (totalRows == 0) return;

  constexpr int kBoxW = 90;
  constexpr int kBoxH = 32;
  constexpr int kGap = 8;
  constexpr int kSidePadding = 12;
  constexpr int kLineGap = 6;
  constexpr int kDividerPad = 10;

  const int boxLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int nameLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int subLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int rowContentH = std::max(boxLineH, kBoxH) + kLineGap + subLineH;
  const int rowStep = rowContentH + kDividerPad * 2 + 1;

  const int pageItems = std::max(1, height / rowStep);
  int pageStart = selectedResultsRow - (pageItems - 1) / 2;
  pageStart = std::max(0, std::min(pageStart, std::max(0, totalRows - pageItems)));

  int rowY = y;
  for (int i = pageStart; i < totalRows && i < pageStart + pageItems; i++) {
    const auto& m = resultsMatches[i];
    const bool selected = (i == selectedResultsRow);
    const bool isLive = (m.state == "in");
    const bool isPre = (m.state == "pre");

    if (selected) {
      renderer.fillRect(x, rowY - kDividerPad / 2, width, rowContentH + kDividerPad, true);
    }

    const int boxX = x + (width - kBoxW) / 2;
    const int boxY = rowY;
    const bool boxFilledBlack = isLive && !selected;
    if (boxFilledBlack) {
      renderer.fillRoundedRect(boxX, boxY, kBoxW, kBoxH, 4, Color::Black);
    } else {
      if (selected) {
        renderer.fillRoundedRect(boxX, boxY, kBoxW, kBoxH, 4, Color::White);
      }
      renderer.drawRoundedRect(boxX, boxY, kBoxW, kBoxH, 1, 4, true);
    }

    const std::string boxText = isPre ? formatEventTimeOnly(m.dateIso) : (m.homeScore + " - " + m.awayScore);
    const int boxTextW = renderer.getTextWidth(UI_12_FONT_ID, boxText.c_str(), EpdFontFamily::BOLD);
    const int boxTextX = boxX + (kBoxW - boxTextW) / 2;
    const int boxTextY = boxY + (kBoxH - boxLineH) / 2;
    renderer.drawText(UI_12_FONT_ID, boxTextX, boxTextY, boxText.c_str(), !boxFilledBlack, EpdFontFamily::BOLD);

    const int leftAvailW = boxX - kGap - (x + kSidePadding);
    const int rightAvailW = (x + width - kSidePadding) - (boxX + kBoxW + kGap);
    const std::string homeTrunc =
        renderer.truncatedText(UI_10_FONT_ID, m.home.c_str(), leftAvailW, EpdFontFamily::REGULAR);
    const std::string awayTrunc =
        renderer.truncatedText(UI_10_FONT_ID, m.away.c_str(), rightAvailW, EpdFontFamily::REGULAR);
    const int homeTruncW = renderer.getTextWidth(UI_10_FONT_ID, homeTrunc.c_str(), EpdFontFamily::REGULAR);
    const int nameTextY = boxY + (kBoxH - nameLineH) / 2;
    renderer.drawText(UI_10_FONT_ID, boxX - kGap - homeTruncW, nameTextY, homeTrunc.c_str(), !selected,
                       EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, boxX + kBoxW + kGap, nameTextY, awayTrunc.c_str(), !selected,
                       EpdFontFamily::REGULAR);

    const std::string subtitle = translateMatchStatus(m.statusDesc) + "  " + formatEventDateOnly(m.dateIso);
    const int subTextW = renderer.getTextWidth(SMALL_FONT_ID, subtitle.c_str());
    const int subTextX = x + (width - subTextW) / 2;
    const int subTextY = boxY + kBoxH + kLineGap;
    renderer.drawText(SMALL_FONT_ID, subTextX, subTextY, subtitle.c_str(), !selected);

    rowY += rowStep;
    const bool hasNextVisibleRow = (i + 1 < totalRows) && (i + 1 < pageStart + pageItems);
    if (hasNextVisibleRow) {
      const int lineY = rowY - kDividerPad - 1;
      renderer.drawLine(x + kSidePadding, lineY, x + width - kSidePadding, lineY, 1, true);
    }
  }
}

std::string FootballActivity::resultsDayLabel() const {
  if (resultsDayOffset == 0) return tr(STR_FOOTBALL_DAY_TODAY);
  if (resultsDayOffset == 1) return tr(STR_FOOTBALL_DAY_YESTERDAY);
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_FOOTBALL_DAY_N_AGO), resultsDayOffset);
  return buf;
}

void FootballActivity::drawTabStrip(int y, const std::vector<std::string>& labels, int selectedFlatIndex) {
  const auto pageWidth = renderer.getScreenWidth();
  const int totalTabs = static_cast<int>(labels.size());
  constexpr int kMaxVisibleTabs = 4;
  const int windowSize = std::min(totalTabs, kMaxVisibleTabs);
  int windowStart = selectedFlatIndex - windowSize / 2;
  windowStart = std::max(0, std::min(windowStart, totalTabs - windowSize));

  const int tabW = (pageWidth - 40) / windowSize;
  constexpr int kTabH = 30;
  const int tabTextY = y + (kTabH - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
  for (int i = 0; i < windowSize; i++) {
    const int labelIndex = windowStart + i;
    const bool active = (labelIndex == selectedFlatIndex);
    const int tx = 20 + i * tabW;
    renderer.drawRoundedRect(tx + 2, y, tabW - 4, kTabH, 1, 5, true);
    if (active) {
      renderer.fillRoundedRect(tx + 2, y, tabW - 4, kTabH, 5, Color::Black);
    }
    const std::string& label = labels[labelIndex];
    const auto truncated = renderer.truncatedText(SMALL_FONT_ID, label.c_str(), tabW - 8);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, truncated.c_str());
    renderer.drawText(SMALL_FONT_ID, tx + (tabW - textW) / 2, tabTextY, truncated.c_str(), !active);
  }
}

void FootballActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == FootballState::LeagueSelection) {
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FOOTBALL_TITLE));

    const int totalItems = static_cast<int>(subscriptions.size()) + 1;
    GUI.drawButtonMenu(
        renderer,
        Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
             pageHeight - (metrics.headerHeight + metrics.topPadding + metrics.verticalSpacing + metrics.buttonHintsHeight)},
        totalItems, selectedSubIndex,
        [this, totalItems](int index) {
          if (index == totalItems - 1) return std::string(tr(STR_FOOTBALL_ADD_LEAGUE));
          return subscriptions[index].displayName;
        },
        [this, totalItems](int index) { return index == totalItems - 1 ? UIIcon::File : UIIcon::Football; });

    const char* rightAction =
        (selectedSubIndex >= 0 && selectedSubIndex < totalItems - 1) ? tr(STR_FOOTBALL_DELETE) : nullptr;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, rightAction);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FootballState::AddLeague) {
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FOOTBALL_ADD_LEAGUE));

    const int totalAvailable = static_cast<int>(addLeagueAvailableIndices.size());
    if (totalAvailable == 0) {
      const int textY = metrics.topPadding + metrics.headerHeight + 60;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_FOOTBALL_ALL_ADDED));
    } else {
      GUI.drawButtonMenu(
          renderer,
          Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
               pageHeight - (metrics.headerHeight + metrics.topPadding + metrics.verticalSpacing + metrics.buttonHintsHeight)},
          totalAvailable, selectedAddIndex,
          [this](int index) { return std::string(AVAILABLE_LEAGUES[addLeagueAvailableIndices[index]].name); },
          [](int) { return UIIcon::Football; });
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), totalAvailable > 0 ? tr(STR_SELECT) : nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const std::string title = subscriptions[activeLeagueIndex].displayName;
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

    const int tabBarY = metrics.topPadding + metrics.headerHeight;

    // When Standings has more than one group (zone), splice its group names
    // into the tab strip after "Results" instead of a single "Standings"
    // tab, so each group reads as its own page.
    std::vector<std::string> tabLabels;
    tabLabels.emplace_back(tr(STR_FOOTBALL_TAB_RESULTS));
    int selectedFlatIndex = 0;
    if (standingsGroups.size() > 1) {
      for (const auto& group : standingsGroups) tabLabels.push_back(group.name);
      if (currentTab == FootballTab::Standings) selectedFlatIndex = 1 + selectedGroupIndex;
    } else {
      tabLabels.emplace_back(tr(STR_FOOTBALL_TAB_STANDINGS));
      if (currentTab == FootballTab::Standings) selectedFlatIndex = 1;
    }
    drawTabStrip(tabBarY + 20, tabLabels, selectedFlatIndex);

    const int contentTop = tabBarY + 20 + 30 + metrics.verticalSpacing;
    const int tab = static_cast<int>(currentTab);
    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

    // Results only ever shows one day at a time (see apiUrl/changeResultsDay),
    // so surface which day that is - otherwise an empty/single-match day looks
    // identical to every other day.
    int listTop = contentTop;
    if (currentTab == FootballTab::Results) {
      const std::string dayLabel = resultsDayLabel();
      const int dayLabelW = renderer.getTextWidth(SMALL_FONT_ID, dayLabel.c_str());
      renderer.drawText(SMALL_FONT_ID, (pageWidth - dayLabelW) / 2, contentTop, dayLabel.c_str(), true);
      listTop = contentTop + renderer.getLineHeight(SMALL_FONT_ID) + 6;
    }
    const Rect listRect{0, listTop, pageWidth, listBottom - listTop};

    if (refreshing[tab]) {
      const int textY = listTop + (listBottom - listTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_FOOTBALL_REFRESHING));
    } else if (!loaded[tab]) {
      const char* msg = !errorMessage[tab].empty() ? errorMessage[tab].c_str() : tr(STR_FOOTBALL_LOADING);
      // drawCenteredText is single-line only; wrap long error text (e.g.
      // STR_FOOTBALL_NO_DATA) instead of letting it overflow the screen edge.
      const int errWidth = pageWidth - 2 * metrics.contentSidePadding;
      auto errLines = renderer.wrappedText(UI_12_FONT_ID, msg, errWidth, 2, EpdFontFamily::REGULAR);
      const int errLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      int errY = listTop + (listBottom - listTop) / 2 - (static_cast<int>(errLines.size()) * errLineHeight) / 2;
      for (const auto& line : errLines) {
        renderer.drawCenteredText(UI_12_FONT_ID, errY, line.c_str());
        errY += errLineHeight;
      }
    } else if (currentTab == FootballTab::Standings) {
      if (!standingsGroups.empty()) {
        const auto& tabRows = standingsGroups[selectedGroupIndex].rows;
        GUI.drawList(
            renderer, listRect, static_cast<int>(tabRows.size()), selectedGroupRow,
            [&tabRows](int i) { return tabRows[i].title; }, nullptr, nullptr,
            [&tabRows](int i) { return tabRows[i].value; }, true);
      }
    } else if (resultsMatches.empty()) {
      // A single day legitimately having no matches for this league is the
      // common case (most leagues don't play daily) - parseAndStore marks
      // this tab loaded rather than erroring, so this is not the same
      // "couldn't load" state above; say so explicitly instead of just
      // leaving the list area blank.
      const int textY = listTop + (listBottom - listTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_FOOTBALL_NO_MATCHES_DAY));
      // Small/subtle on purpose - only relevant here, where the list is
      // genuinely empty; it must not compete with the main message or show
      // up once real matches are on screen.
      const int hintY = textY + renderer.getLineHeight(UI_12_FONT_ID) + 6;
      renderer.drawCenteredText(SMALL_FONT_ID, hintY, tr(STR_FOOTBALL_CHANGE_DAY_HINT));
    } else {
      drawResultsList(listRect.x, listRect.y, listRect.width, listRect.height);
    }

    if (refreshFailed[tab]) {
      GUI.drawPopup(renderer, tr(STR_FOOTBALL_REFRESH_FAILED));
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_FOOTBALL_REFRESH), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

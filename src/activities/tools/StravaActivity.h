#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

enum class StravaTab { Stats = 0, Activities = 1 };
constexpr int STRAVA_TAB_COUNT = 2;

// One sport's totals from Strava's /athletes/{id}/stats endpoint: count,
// distance (meters), moving time (seconds), elevation gain (meters). That
// endpoint only ever returns Run/Ride/Swim totals - Strava's own limitation,
// not something this firmware can widen without aggregating the whole
// activities list client-side. Other sport types still show up fine in the
// Activities tab and its detail view, just aren't part of this dashboard.
struct StravaSportTotals {
  int count = 0;
  double distanceMeters = 0;
  int movingTimeSec = 0;
  double elevationGainMeters = 0;
};

// Indexed by sport: 0=Run, 1=Ride, 2=Swim (see StravaActivity::selectedSport).
struct StravaStats {
  StravaSportTotals recent[3];   // last 4 weeks
  StravaSportTotals ytd[3];
  StravaSportTotals allTime[3];
};

struct StravaActivityItem {
  std::string name;
  std::string sportType;       // raw Strava sport_type, e.g. "Run", "Ride", "TrailRun"
  double distanceMeters = 0;
  int movingTimeSec = 0;
  double elevationGainMeters = 0;
  std::string startDateLocal;  // ISO, e.g. "2026-08-19T07:12:00"
  double averageSpeed = 0;     // m/s
  int kudosCount = 0;
};

// Network fetches are synchronous (block behind a "Loading"/"Refreshing"
// state) - same reasoning as FormulaOneActivity: activities in this codebase
// don't spawn their own tasks, the render task is already the only
// background task in the process.
class StravaActivity final : public Activity {
 private:
  StravaTab currentTab = StravaTab::Stats;
  int selectedRow[STRAVA_TAB_COUNT] = {0, 0};  // only selectedRow[Activities] is used
  bool loaded[STRAVA_TAB_COUNT] = {false, false};
  bool refreshing[STRAVA_TAB_COUNT] = {false, false};
  // Set when a manual refresh fails while loaded[tab] was already true, so
  // the old data stays on screen but the user still sees it didn't update.
  bool refreshFailed[STRAVA_TAB_COUNT] = {false, false};
  std::string errorMessage[STRAVA_TAB_COUNT];

  // Stats tab cycles Run/Ride/Swim with Up/Down - see StravaSportTotals.
  int selectedSport = 0;
  StravaStats stats;

  std::vector<StravaActivityItem> recentActivities;

  // Athlete id/first name - fetched once (via /athlete) and cached
  // indefinitely on SD, since an athlete's id never changes. Not modeled as
  // a tab: it's not user-navigable, just a prerequisite for the Stats tab's
  // /athletes/{id}/stats URL.
  std::string athleteId;
  std::string athleteFirstName;
  bool athleteLoaded = false;

  // Detail drill-down for a selected Activities row, reached by tapping
  // Confirm - reuses fields already fetched with the list, no extra request.
  bool showingActivityDetail = false;
  int detailActivityIndex = -1;
  // Swallows the Confirm release that ends a hold-to-refresh, so it doesn't
  // also open the detail view for the row under the cursor - same idiom as
  // FormulaOneActivity's driverRefreshHoldFired.
  bool refreshHoldFired = false;

  // Set once a fetch actually reaches the network, so onExit() only pays for
  // a heap-defrag reboot when this session actually used WiFi.
  bool wifiWasUsed = false;

  bool ensureAccessToken();
  bool ensureAthlete();
  bool loadAthleteFromSd();

  void startFetch(int tab);
  void doFetch(int tab);
  bool loadCacheFromSd(int tab);
  bool parseStats(const std::string& json);
  bool parseActivities(const std::string& json);
  std::string cachePath(int tab) const;
  std::string apiUrl(int tab) const;

  void openActivityDetail(int index);

  // No GUI.drawTabBar in this theme - draws a small row of rounded-rect tabs
  // directly, same look FormulaOneActivity/FootballActivity already use.
  void drawTabStrip(int y, const std::vector<std::string>& labels, int selectedIndex);

 public:
  explicit StravaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Strava", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

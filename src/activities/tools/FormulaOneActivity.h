#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// SessionSchedule and Results/Qualifying/Sprint/SessionResult aren't real
// tabs in the bar — confirming a Calendar row always opens SessionSchedule
// first, listing every session of that weekend (past or future, finished or
// not). Once a session's own time has passed, its row shows "Ver" instead of
// the scheduled time and can be confirmed to drill into that one session's
// results. Race/Qualifying/Sprint drill into their own dedicated tab (stable
// jolpica-f1 Ergast-compatible API, one request); Practice and Sprint
// Qualifying drill into the shared SessionResult tab instead (jolpica-f1's
// newer, unversioned "alpha" API — the stable API never exposed those
// sessions' results at all), parameterized per entry by which alpha session
// code to fetch (see enterAlphaSessionResult).
enum class F1Tab {
  Drivers = 0,
  Constructors = 1,
  Results = 2,
  Calendar = 3,
  SessionSchedule = 4,
  Qualifying = 5,
  Sprint = 6,
  SessionResult = 7,
};
constexpr int F1_TAB_COUNT = 8;

struct F1Row {
  std::string title;
  std::string subtitle;
  std::string value;
};

// Practice/qualifying/sprint session times for one race weekend, parallel to
// calendarRounds. Pulled straight out of the Calendar tab's
// own fetch (the schedule endpoint already includes these), so showing them
// needs no extra network request. Sprint fields are empty on a non-sprint
// weekend; FP2/FP3 are empty on a sprint weekend (only FP1 is scheduled).
struct F1RaceWeekend {
  std::string fp1Date, fp1Time;
  std::string fp2Date, fp2Time;
  std::string fp3Date, fp3Time;
  std::string sprintQualDate, sprintQualTime;
  std::string sprintDate, sprintTime;
  std::string qualDate, qualTime;
  std::string raceDate, raceTime;
};

// Bio fields for one driver, parallel to rows[Drivers] (same order/index).
// Pulled from the same driverstandings response already fetched for that
// tab — Ergast/Jolpica's Driver object already carries all of these, so no
// extra network request is needed to have them on hand.
struct F1DriverBio {
  std::string number;
  std::string code;
  std::string nationality;
  std::string dateOfBirth;
  std::string wikiUrl;
};

// Network fetches are synchronous (block the main loop behind a "Loading"/
// "Refreshing" state, same as FontDownloadActivity) instead of the
// background-FreeRTOS-task pattern the source app used — activities in this
// codebase are not supposed to spawn their own tasks (see
// docs/activity-manager.md), and the render task is already the only
// background task in the process.
class FormulaOneActivity final : public Activity {
 private:
  F1Tab currentTab = F1Tab::Drivers;
  int selectedRow[F1_TAB_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0};  // remembered scroll position per tab
  bool loaded[F1_TAB_COUNT] = {false, false, false, false, false, false, false, false};
  std::string errorMessage[F1_TAB_COUNT];
  bool refreshing[F1_TAB_COUNT] = {false, false, false, false, false, false, false, false};
  // Set when a manual refresh fails while `loaded[tab]` was already true, so
  // the old data stays on screen but the user still sees that it didn't
  // update, instead of the refresh failing in total silence.
  bool refreshFailed[F1_TAB_COUNT] = {false, false, false, false, false, false, false, false};
  std::vector<F1Row> rows[F1_TAB_COUNT];
  std::string raceName;  // Results tab sub-header (data from the API, not i18n)
  std::string raceDate;

  // Bio fields for rows[Drivers], same order/index — see F1DriverBio.
  std::vector<F1DriverBio> driverBios;

  // Driver detail view, reached by tapping (not holding) Confirm on a Drivers
  // row. Not modeled as another F1Tab like Results/Qualifying/Sprint are:
  // it's a single record, not a fetchable list, so it gets its own small
  // state instead of a slot in the per-tab arrays above.
  bool showingDriverDetail = false;
  int detailDriverIndex = -1;
  // Swallows the Confirm release that ends a hold-to-refresh, so it doesn't
  // also open the detail view for the row under the cursor — same idiom as
  // CalendarActivity's holidayHoldFired.
  bool driverRefreshHoldFired = false;
  void openDriverDetail(int index);

  // Same hold-to-refresh idiom as driverRefreshHoldFired — Calendar's tap-
  // Confirm is already spoken for (opens SessionSchedule), so refresh moves
  // to a hold instead of a dedicated key or hint.
  bool calendarRefreshHoldFired = false;

  // Results normally shows the latest race ("current/last/results/", the
  // -1 case). Confirming a SessionSchedule "Ver" row instead drills into
  // that specific round, reusing the same Results slot/cache mechanism
  // parameterized by round number.
  int selectedRound = -1;
  // Sets selectedRound/currentTab, clears the session-detail tabs' stale
  // state, and loads targetTab — reached only by confirming a SessionSchedule
  // "Ver" row (Calendar confirm always opens the schedule first, see
  // loop()), so Back out of a session always returns there.
  void enterSessionResults(F1Tab targetTab, int round);
  // Same shape as enterSessionResults, but for a Practice/Sprint-Qualifying
  // row: those drill into the shared SessionResult tab instead of their own
  // dedicated one, parameterized by which alpha session code to fetch (see
  // doFetchSessionResult) and the localized label shown as its subheader
  // subtitle (there's no single unambiguous date to show there, unlike
  // Results/Qualifying/Sprint).
  void enterAlphaSessionResult(int round, const std::string& alphaCode, const std::string& label);
  // Shared by enterSessionResults/enterAlphaSessionResult: wipes stale state
  // for every "drill into one session" tab whenever any one of them is
  // (re-)entered, since they all key off the shared selectedRound/raceName
  // that's about to change.
  void clearSessionDetailTabsState();
  // Two-step fetch for F1Tab::SessionResult — jolpica-f1's alpha API has no
  // single "give me round X's session Y results" endpoint: the round number
  // this app already tracks must first be resolved to an opaque round_id
  // (GET .../alpha/core/rounds/?year=&round_number=), then that round_id is
  // used to fetch the actual results (GET .../alpha/results/{round_id}/
  // {code}/). Called from doFetch() when tab == SessionResult.
  void doFetchSessionResult();
  // alpha session_filter code for the session currently being fetched/shown
  // on the SessionResult tab, e.g. "FP1"/"FP2"/"FP3"/"SQ".
  std::string sessionResultCode;
  // Localized session name (e.g. "Practice 1"), shown as the SessionResult
  // subheader's subtitle in render().
  std::string sessionResultLabel;
  // 4-digit season year for the round currently being fetched — the alpha
  // round-lookup needs it, but nothing else in this file tracks a season
  // year explicitly (round-based Ergast URLs use the literal "current"), so
  // it's derived from the weekend's own raceDate at entry time instead of
  // being stored anywhere else.
  std::string sessionResultYear;
  // Opaque round_id resolved by doFetchSessionResult()'s first step; empty
  // until that step succeeds. Not cached across app restarts — cheap and
  // transient, only needed to build the second step's URL.
  std::string f1AlphaRoundId;

  // Round number per row in rows[Calendar], same order/index — lets Confirm
  // know which round to fetch.
  std::vector<int> calendarRounds;
  std::vector<F1RaceWeekend> calendarWeekends;  // session times, same order/index
  std::string sessionScheduleRaceName;          // subheader while viewing SessionSchedule
  // calendarWeekends/calendarRounds index the weekend the SessionSchedule
  // screen currently on display was built from — needed to resolve the round
  // number when a schedule row itself is confirmed, and to rebuild the same
  // schedule when Back returns to it.
  int sessionScheduleCalendarIdx = -1;
  // Per rows[SessionSchedule] entry, same order/index: which tab Confirm
  // drills into, plus (only meaningful when tab == SessionResult) the alpha
  // session_filter code and the localized session label used as that view's
  // subheader subtitle. tab == F1Tab::SessionSchedule itself is the sentinel
  // for "not viewable" (session hasn't happened yet).
  struct F1SessionScheduleTarget {
    F1Tab tab;
    std::string alphaCode;
    std::string label;
  };
  std::vector<F1SessionScheduleTarget> sessionScheduleTargets;
  // One-shot: the first time the Calendar loads, jump the cursor to the most
  // recent already-run race so it's immediately selectable without
  // scrolling. Doesn't fight the user's own scrolling on later visits.
  bool calendarCursorPositioned = false;
  void positionCalendarCursorOnLastPastRace();

  // Set once a fetch actually reaches HttpDownloader, so onExit() only pays
  // for a heap-defrag reboot when this session actually used WiFi.
  bool wifiWasUsed = false;
  // Builds rows[SessionSchedule] from calendarWeekends[calendarIdx] and
  // switches currentTab to it — synchronous, no fetch involved.
  void showSessionSchedule(int calendarIdx);

  void startFetch(int tab);
  void doFetch(int tab);
  bool loadCacheFromSd(int tab);
  void parseAndStore(int tab, const std::string& json);
  std::string cachePath(int tab) const;
  std::string apiUrl(int tab) const;

  // No GUI.drawTabBar in this theme (the source app's theme grew one this
  // codebase doesn't have) — draws a small row of rounded-rect tabs directly,
  // same look DiceActivity/FootballActivity already use.
  void drawTabStrip(int y, const std::vector<std::string>& labels, int selectedIndex);

 public:
  explicit FormulaOneActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FormulaOne", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

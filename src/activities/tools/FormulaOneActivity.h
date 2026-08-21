#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// SessionSchedule and Results/Qualifying/Sprint aren't real tabs in the bar —
// confirming a Calendar row always opens SessionSchedule first, listing every
// session of that weekend (past or future, finished or not). Once a
// session's own time has passed, its row shows "Ver" instead of the
// scheduled time and can be confirmed to drill into that one session's
// results (Results/Qualifying/Sprint, whichever it is). Practice sessions
// never get a "Ver": the data source (jolpica-f1) doesn't expose practice
// results.
enum class F1Tab { Drivers = 0, Constructors = 1, Results = 2, Calendar = 3, SessionSchedule = 4, Qualifying = 5, Sprint = 6 };
constexpr int F1_TAB_COUNT = 7;

struct F1Row {
  std::string title;
  std::string subtitle;
  std::string value;
};

// Practice/qualifying/sprint session times for one race weekend, parallel to
// calendarRounds/calendarDatesIso. Pulled straight out of the Calendar tab's
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
  int selectedRow[F1_TAB_COUNT] = {0, 0, 0, 0, 0, 0, 0};  // remembered scroll position per tab
  bool loaded[F1_TAB_COUNT] = {false, false, false, false, false, false, false};
  std::string errorMessage[F1_TAB_COUNT];
  bool refreshing[F1_TAB_COUNT] = {false, false, false, false, false, false, false};
  // Set when a manual refresh fails while `loaded[tab]` was already true, so
  // the old data stays on screen but the user still sees that it didn't
  // update, instead of the refresh failing in total silence.
  bool refreshFailed[F1_TAB_COUNT] = {false, false, false, false, false, false, false};
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

  // Results normally shows the latest race ("current/last/results/", the
  // -1 case). Confirming a SessionSchedule "Ver" row instead drills into
  // that specific round, reusing the same Results slot/cache mechanism
  // parameterized by round number.
  int selectedRound = -1;
  // Sets selectedRound/currentTab, clears the three session tabs' stale
  // state, and loads targetTab — reached only by confirming a SessionSchedule
  // "Ver" row (Calendar confirm always opens the schedule first, see
  // loop()), so Back out of a session always returns there.
  void enterSessionResults(F1Tab targetTab, int round);

  // Round number and raw ISO date per row in rows[Calendar], same order/index
  // — round lets Confirm know which round to fetch; date is what decides
  // whether a race is done yet (compared against today).
  std::vector<int> calendarRounds;
  std::vector<std::string> calendarDatesIso;
  std::vector<F1RaceWeekend> calendarWeekends;  // session times, same order/index
  std::string sessionScheduleRaceName;          // subheader while viewing SessionSchedule
  // calendarWeekends/calendarRounds index the weekend the SessionSchedule
  // screen currently on display was built from — needed to resolve the round
  // number when a schedule row itself is confirmed, and to rebuild the same
  // schedule when Back returns to it.
  int sessionScheduleCalendarIdx = -1;
  // Target tab to drill into when each rows[SessionSchedule] entry is
  // confirmed, same order/index. F1Tab::SessionSchedule itself is the
  // sentinel for "not viewable" (session hasn't happened yet, or is a
  // practice session the API never provides results for).
  std::vector<F1Tab> sessionScheduleTargets;
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

#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class FootballState { LeagueSelection, AddLeague, LeagueDetail };
enum class FootballTab { Results = 0, Standings = 1 };

struct FootballLeague {
  std::string slug;         // ESPN league slug, e.g. "arg.1"
  std::string displayName;  // e.g. "Liga Profesional Argentina"
};

struct FootballRow {
  std::string title;
  std::string subtitle;
  std::string value;
};

// Results keeps the raw fields (not a pre-baked title string) so the row
// renderer can lay the score out in its own centered box instead of just
// concatenating "Home 1 - 4 Away" as plain text.
struct FootballMatch {
  std::string home;
  std::string away;
  std::string homeScore;
  std::string awayScore;
  std::string state;       // ESPN status.type.state: "pre" | "in" | "post"
  std::string statusDesc;  // e.g. "Full Time", "Scheduled", "1st Half"
  std::string dateIso;     // raw ESPN date; formatted (and timezone-shifted) at render time
};

// Standings are organized into zones/groups by ESPN (e.g. "Grupo A"/"Grupo B"
// during a Libertadores group stage). Ungrouped leagues still come back as a
// single group, so callers don't need to special-case the count.
struct FootballGroup {
  std::string name;
  std::vector<FootballRow> rows;
};

// Network fetches are synchronous (block the main loop behind a "Refreshing"
// state, same as FontDownloadActivity) instead of the background-FreeRTOS-task
// pattern the source app used — activities in this codebase are not supposed
// to spawn their own tasks (see docs/activity-manager.md), and the render
// task is already the only background task in the process.
class FootballActivity final : public Activity {
 private:
  FootballState state = FootballState::LeagueSelection;
  FootballTab currentTab = FootballTab::Results;

  std::vector<FootballLeague> subscriptions;
  std::vector<int> addLeagueAvailableIndices;  // indices into kAvailableLeagues not yet subscribed
  int selectedSubIndex = 0;
  int selectedAddIndex = 0;
  int activeLeagueIndex = -1;  // index into subscriptions, set when entering LeagueDetail

  bool loaded[2] = {false, false};
  std::string errorMessage[2];
  // Shown while a fetch for that tab is in flight (the fetch itself blocks
  // the main loop, so this only affects what render() draws just before and
  // just after the blocking call, not a live progress indicator).
  bool refreshing[2] = {false, false};
  // Set when a manual refresh fails while `loaded[tab]` was already true, so
  // the old data stays on screen but the user still sees that it didn't
  // update, instead of the refresh failing in total silence.
  bool refreshFailed[2] = {false, false};

  std::vector<FootballMatch> resultsMatches;
  int selectedResultsRow = 0;

  std::vector<FootballGroup> standingsGroups;
  int selectedGroupIndex = 0;  // which group tab is active within Standings
  int selectedGroupRow = 0;    // selected row within the active group

  // Set once a fetch actually reaches HttpDownloader, so onExit() only pays
  // for a heap-defrag reboot when this session actually used WiFi.
  bool wifiWasUsed = false;

  void loadSubscriptions();
  void saveSubscriptions();
  void rebuildAddLeagueList();

  void startFetch(int tab);
  void doFetch(int tab);
  bool loadCacheFromSd(int tab);
  void parseAndStore(int tab, HalFile& file);
  std::string cachePath(int tab) const;
  std::string tmpPath(int tab) const;
  std::string apiUrl(int tab) const;

  // Hand-rolled row rendering for Results (team names + a centered
  // score/kickoff-time box + a divider between matches) — GUI.drawList only
  // does plain title/subtitle/value text, not a custom-laid-out row.
  void drawResultsList(int x, int y, int width, int height);
  // No GUI.drawTabBar in this theme (the source app's theme grew one this
  // codebase doesn't have) — draws a small row of rounded-rect tabs directly,
  // same look DiceActivity already uses for its 5 dice-mode tabs.
  void drawTabStrip(int y, const std::vector<std::string>& labels, int selectedFlatIndex);

 public:
  explicit FootballActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Football", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

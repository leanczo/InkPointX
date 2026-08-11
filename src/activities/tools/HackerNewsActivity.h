#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class HNState { CategoryList, Loading, StoryDetail };
enum class HNTab { Top = 0, New, Ask, Show };
constexpr int HN_TAB_COUNT = 4;

struct HNStory {
  std::string id;
  std::string title;
  std::string url;  // empty for self-text posts (e.g. most Ask HN)
  std::string author;
  int points = 0;
  int numComments = 0;
  std::string relativeTime;
};

// Network fetches are synchronous (block the main loop behind a "Loading"/
// "Refreshing" state, same as FootballActivity/CalendarActivity) instead of
// the source app's background-FreeRTOS-task pattern.
//
// "Open Link" (downloading the story's external URL and displaying its raw
// HTML) is not ported: TxtReaderActivity here has no tag-aware rendering, so
// dumping an arbitrary external page's markup into it would just print
// visible <tags> and &entities; instead of readable text. "View Comments"
// stays faithful by converting the Algolia comment tree to indented plain
// text (see htmlToPlainText() in the .cpp) rather than the source's
// HTML-with-<blockquote> output. "Send to Phone" is unaffected — it just
// QR-encodes the URL.
class HackerNewsActivity final : public Activity {
 private:
  HNState state = HNState::CategoryList;
  HNTab currentTab = HNTab::Top;

  bool loaded[HN_TAB_COUNT] = {false, false, false, false};
  // Set when a manual refresh fails while `loaded[tab]` was already true, so
  // the old data stays on screen but the user still sees that it didn't
  // update, instead of the refresh failing in total silence.
  bool refreshFailed[HN_TAB_COUNT] = {false, false, false, false};
  std::string errorMessage[HN_TAB_COUNT];
  std::vector<HNStory> stories[HN_TAB_COUNT];
  int selectedRow[HN_TAB_COUNT] = {0, 0, 0, 0};

  int detailStoryIndex = -1;  // index into stories[currentTab], set when entering StoryDetail
  int detailMenuIndex = 0;

  // Stashed across the async WiFi-connect prompt in openComments(), since the
  // selected story pointer itself isn't safe to hold across that gap.
  std::string pendingCommentsStoryId;
  std::string pendingCommentsStoryTitle;
  // Distinguishes a comments-fetch Loading screen from a tab-list Loading
  // screen (both reuse HNState::Loading) so render() shows the right message.
  bool fetchingComments = false;
  // Set when doFetchComments() fails, so the StoryDetail render (which it
  // returns to) can surface a popup instead of failing in total silence.
  bool commentsFetchFailed = false;

  void startFetch(int tab);
  void doFetch(int tab);
  bool loadCacheFromSd(int tab);
  void parseAndStoreList(int tab, HalFile& file);
  std::string cachePath(int tab) const;
  std::string tmpPath(int tab) const;
  std::string apiUrl(int tab) const;

  const HNStory* selectedStory() const;
  void openComments();
  void ensureWifiThenFetchComments();
  void doFetchComments();
  bool fetchAndWriteComments(const std::string& storyId, const std::string& storyTitle,
                             const std::string& destPath) const;
  void pushCommentsReader(const std::string& path);
  void showQrForLink();

  void drawTabStrip(int y, int selectedTab) const;

 public:
  explicit HackerNewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HackerNews", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};

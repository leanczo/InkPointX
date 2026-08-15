#pragma once

#include <DateMath.h>
#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class OnThisDayState { List, Loading, EntryDetail };
enum class OnThisDayCategory { Events = 0, Births = 1, Deaths = 2 };
constexpr int OTD_CATEGORY_COUNT = 3;

struct OnThisDayEntry {
  int year = 0;
  std::string text;
  std::string pageUrl;  // pages[0].content_urls.desktop.page; may be empty
};

// Network fetches are synchronous (block the main loop behind a "Loading"/
// "Refreshing" state, same as FootballActivity) instead of the source app's
// background-FreeRTOS-task pattern.
class OnThisDayActivity final : public Activity {
 private:
  OnThisDayState state = OnThisDayState::List;
  OnThisDayCategory currentCategory = OnThisDayCategory::Events;
  // Only month/day are ever read; year is a fixed anchor so DateMath's day
  // arithmetic has a valid CivilDate to round-trip through and is never
  // itself displayed or sent to the API (the onthisday feed is year-independent).
  DateMath::CivilDate viewedDate;

  bool loaded[OTD_CATEGORY_COUNT] = {false, false, false};
  // Shown while a fetch for that category is in flight (the fetch itself
  // blocks the main loop, so this only affects what render() draws just
  // before and just after the blocking call).
  bool refreshing[OTD_CATEGORY_COUNT] = {false, false, false};
  bool refreshFailed[OTD_CATEGORY_COUNT] = {false, false, false};
  std::string errorMessage[OTD_CATEGORY_COUNT];
  std::vector<OnThisDayEntry> entries[OTD_CATEGORY_COUNT];
  // 0=Refresh, 1=PrevDay, 2=NextDay, 3+ = entries[cat][selectedRow-3]
  int selectedRow[OTD_CATEGORY_COUNT] = {0, 0, 0};
  int scrollOffset[OTD_CATEGORY_COUNT] = {0, 0, 0};  // first visible entry index

  // EntryDetail state: index into entries[currentCategory] for the entry
  // being read in full, plus its own line-based scroll (paged by whatever
  // detailMaxLines worked out to at last render, same pattern as
  // RssActivity's PostDetail).
  int detailEntryIndex = -1;
  int detailScrollOffset = 0;
  int detailMaxLines = 1;

  // Lowercased Wikipedia language code, e.g. "es"; may flip to "en" after a
  // 404 (see doFetch's one-time language-fallback probe).
  std::string activeLangCode;
  bool langFallbackProbed = false;

  void startFetch(int category);
  void doFetch(int category);
  bool loadCacheFromSd(int category);
  void parseAndStore(int category, HalFile& file);
  std::string cachePath(int category) const;
  std::string tmpPath(int category) const;
  std::string apiUrl(int category) const;

  void changeDate(int deltaDays);
  void showQrForSelected();
  std::string formattedHeaderDate() const;
  void drawTabStrip(int y, int selectedTab) const;

 public:
  explicit OnThisDayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OnThisDay", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};

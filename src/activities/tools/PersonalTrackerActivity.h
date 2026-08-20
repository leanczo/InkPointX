#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

struct TrackedReview {
  std::string id;          // Firebase push key, e.g. "-NxAbCdEf123" - used to fetch this review's full text on demand.
  std::string title;
  std::string category;   // Raw lean-reviews category, e.g. "book", "videogame".
  std::string createdAt;  // "YYYY-MM-DD" - lexicographically sortable as-is.
  int rating = 0;         // 1-10.
};

// Shows the last 20 entries from the user's own lean-reviews Firebase
// Realtime Database - a read-only "what did I last watch/read/play" list.
// Network fetch is synchronous (blocks the main loop behind a "Refreshing"
// state), same pattern as every other tools/ activity - see SismosActivity.
// The /reviews node's Firebase rule is public-read, so this is a plain HTTPS
// GET with no SDK and no credentials stored on the device.
class PersonalTrackerActivity final : public Activity {
 private:
  bool loaded = false;
  bool refreshing = false;
  // Set when a manual refresh fails while `loaded` was already true, so the
  // old list stays on screen but the user still sees that it didn't update.
  bool refreshFailed = false;
  std::string errorMessage;
  std::vector<TrackedReview> reviews;
  int selectedRow = 0;

  // Set once a fetch actually reaches HttpDownloader, so onExit() only pays
  // for a heap-defrag reboot when this session actually used WiFi.
  bool wifiWasUsed = false;

  // Full-review "detail" view, entered by pressing Confirm/Left on the
  // selected row (still `selectedRow` - the list is never mutated while this
  // is open). summary/content are deliberately excluded from the list fetch
  // above to save RAM (see parseAndStore()), so opening a review fetches
  // just that one review's summary+content on demand.
  bool detailOpen = false;
  bool detailLoading = false;
  std::string detailError;
  std::string detailSummary;
  std::string detailContent;
  int detailScrollOffset = 0;
  int detailMaxLines = 1;

  void startFetch();
  void doFetch();
  bool loadCacheFromSd();
  bool parseAndStore(const std::string& json);
  static std::string cachePath();
  static std::string tmpPath();
  static std::string apiUrl();

  void openDetail(int index);
  void doFetchDetail(const std::string& id);
  bool parseDetailJson(const std::string& json, std::string& outSummary, std::string& outContent);
  static std::string detailApiUrl(const std::string& id);
  static std::string detailTmpPath();

 public:
  explicit PersonalTrackerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PersonalTracker", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

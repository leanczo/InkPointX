#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

struct TrackedReview {
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

  void startFetch();
  void doFetch();
  bool loadCacheFromSd();
  bool parseAndStore(const std::string& json);
  static std::string cachePath();
  static std::string tmpPath();
  static std::string apiUrl();

 public:
  explicit PersonalTrackerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PersonalTracker", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

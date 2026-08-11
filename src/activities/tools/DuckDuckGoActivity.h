#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class DDGState { RecentSearches, SearchResults, Loading };

struct DuckLink {
  std::string title;
  std::string url;
};

// Network fetches are synchronous (block the main loop behind a "Loading"
// state, same as WikipediaActivity) instead of the source app's
// background-FreeRTOS-task pattern.
//
// The source app lets you select a result to download its page and open it
// (raw HTML, tags and all) in the text reader. InkPointX's ReaderActivity
// doesn't dispatch .html files to a text view at all (see
// ReaderActivity::isTxtFile — only .txt/.md), and TxtReaderActivity has no
// tag-aware rendering either, so an arbitrary downloaded page would just
// show visible <tags>. This mirrors the same call already made for
// HackerNewsActivity's "Open Link" and RssActivity's "Visit Link": that
// action is replaced with "Show QR" (encode the result's URL so it can be
// opened on a phone) instead of dropping result-visiting entirely. Since
// nothing downloads pages into /websites anymore, the source app's
// "offline list of previously downloaded pages" landing screen is replaced
// with a small persisted list of recent search queries instead — same
// two-tier list-then-search shape, without depending on the removed
// download path.
class DuckDuckGoActivity final : public Activity {
 private:
  DDGState state = DDGState::RecentSearches;
  std::vector<std::string> recentSearches;
  std::vector<DuckLink> searchResults;

  std::string searchQuery;
  std::string errorMessage;

  int selectedIndex = 0;

  bool wifiWasUsed = false;

  void loadRecentSearches();
  void saveRecentSearches();
  void rememberSearch(const std::string& query);

  void promptSearch();
  void runSearch(const std::string& query);
  void ensureWifiThenFetch();
  void doFetch();
  bool fetchSearchData();
  void showQrForResult(int index);

 public:
  explicit DuckDuckGoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DuckDuckGo", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

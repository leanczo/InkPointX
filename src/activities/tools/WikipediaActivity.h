#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class WikiState { OfflineList, SearchResults, Loading };

// Network fetches are synchronous (block the main loop behind a "Loading"
// state, same as FontDownloadActivity) instead of the background-FreeRTOS-
// task pattern the source app used. The source app's ArticleView state (an
// in-activity text viewer with manual scrolling) is dead code there — every
// article, once downloaded, is opened through TxtReaderActivity instead — so
// it is not ported here either.
class WikipediaActivity final : public Activity {
 private:
  WikiState state = WikiState::OfflineList;
  WikiState stateBeforeFetch = WikiState::OfflineList;  // where to return to if WiFi connect is cancelled
  std::vector<std::string> offlineArticles;
  std::vector<std::string> searchResults;

  std::string currentArticleTitle;

  int selectedIndex = 0;

  std::string searchQuery;
  std::string articleToFetch;
  std::string errorMessage;
  bool isSearchTask = false;
  bool wifiWasUsed = false;

  void loadOfflineArticlesList();
  void promptSearch();
  void openArticle(const std::string& title);
  void ensureWifiThenFetch();
  void doFetch();
  bool fetchSearchData();
  bool fetchArticleData();

 public:
  explicit WikipediaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Wikipedia", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

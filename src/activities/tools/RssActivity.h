#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

struct RssItem {
  std::string title;
  std::string link;
  std::string description;
  std::string content;   // Full article content (if available)
  std::string timestamp;  // Unix timestamp as string
  std::string feedName;  // display name of the feed
};

struct RssSubscription {
  std::string url;
  std::string customName;  // empty = no override, fall back to getFriendlyFeedName(url)
};

enum class RssState {
  FeedSelection,
  Loading,
  FeedList,
  PostDetail,
  // Reached by pressing Right on a subscription row in FeedSelection — a
  // small 3-item menu (Edit Title / Edit URL / Delete Feed), so a single
  // stray Right press can no longer trigger a delete directly.
  FeedActionMenu
};

// Network fetches are synchronous (block the main loop behind a "Loading"/
// "Refreshing" state, same as FootballActivity/HackerNewsActivity) instead of
// the source app's background-FreeRTOS-task pattern.
//
// "Visit Link" (downloading a post's external URL and displaying its raw
// HTML/text) is not ported, for the same reason HackerNewsActivity drops
// "Open Link": TxtReaderActivity has no tag-aware rendering, so dumping an
// arbitrary external page's markup into it would just print visible <tags>.
// "Show QR" (encode the post's URL so it can be opened on a phone) covers the
// same underlying need without that problem, so PostDetail's Confirm goes
// straight to it instead of through the source's now-redundant 2-item
// "Visit Link / Show QR" action menu.
class RssActivity final : public Activity {
 private:
  RssState state = RssState::FeedSelection;
  std::vector<RssItem> allItems;
  std::vector<RssSubscription> subscriptions;
  std::string activeFeed;

  int selectedItemIndex = 0;
  int selectedSubIndex = 0;
  int itemsScrollOffset = 0;
  int detailScrollOffset = 0;
  int feedActionMenuIndex = 0;  // which item is highlighted in FeedActionMenu
  // Lines-per-screen for the article body at the current font size, computed
  // by render() and reused by loop() so Up/Down scroll by a full page
  // instead of a single line.
  int detailMaxLines = 1;
  uint8_t articleFontSizeIndex = 0;  // index into kRssArticleFontIds, persisted across sessions

  std::string errorMessage;
  bool isRefreshing = false;
  bool wifiWasUsed = false;

  void loadSubscriptions();
  void saveSubscriptions();
  // customName if set for `url`, else the auto-derived getFriendlyFeedName(url).
  std::string displayNameForUrl(const std::string& url) const;
  bool loadOfflineFeeds();
  void ensureDirectoriesExist();
  void loadArticleFontSize();
  void saveArticleFontSize();

  bool parseFeedsFromMarkdown(const std::string& filepath, std::vector<RssItem>& targetList, bool summaryOnly = false);

  // Opens `url` as the active feed: shows the cached list immediately if one
  // exists on disk, otherwise falls through to startFetch().
  void openFeed(const std::string& url);
  void promptAddUrl();
  void startFetch();
  void doFetch();
  void showQrForActivePost();

 public:
  explicit RssActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RssFeed", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

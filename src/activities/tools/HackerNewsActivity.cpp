#include "HackerNewsActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Txt.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/reader/TxtReaderActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

namespace {

// Comments nest with unlimited depth on HN; both caps below are defensive
// bounds so a mega-popular thread can't blow the ~380KB heap budget. Replies
// past the depth cap, or comments past the count cap, are simply omitted
// (with a note appended for the count cap) rather than crashing.
constexpr int kMaxCommentDepth = 6;
constexpr int kMaxComments = 200;

std::string formatRelativeTime(int64_t createdAtUnix) {
  time_t now = time(nullptr);
  if (now < static_cast<time_t>(createdAtUnix)) return "";
  int64_t diff = static_cast<int64_t>(now) - createdAtUnix;
  char buf[16];
  if (diff < 60) return tr(STR_HN_TIME_NOW);
  if (diff < 3600) {
    snprintf(buf, sizeof(buf), tr(STR_HN_TIME_MINUTES), static_cast<int>(diff / 60));
  } else if (diff < 86400) {
    snprintf(buf, sizeof(buf), tr(STR_HN_TIME_HOURS), static_cast<int>(diff / 3600));
  } else {
    snprintf(buf, sizeof(buf), tr(STR_HN_TIME_DAYS), static_cast<int>(diff / 86400));
  }
  return buf;
}

// Builds a fixed-depth recursive filter so ArduinoJson only keeps the fields
// we need from an arbitrarily deep (but capped) comment tree - Filter can
// prune known field *paths*, so unlimited recursion is approximated by
// repeating the same shape kMaxCommentDepth times; anything deeper is simply
// absent from the parsed document.
void buildCommentFilter(JsonDocument& filter) {
  JsonObject node = filter.to<JsonObject>();
  node["title"] = true;
  node["text"] = true;
  node["author"] = true;
  for (int depth = 0; depth < kMaxCommentDepth; depth++) {
    JsonObject child = node["children"][0].to<JsonObject>();
    child["author"] = true;
    child["text"] = true;
    node = child;
  }
}

// The Algolia API's "text" fields are raw HN comment HTML (paragraphs as
// <p>, links as <a href>, entities as &quot;/&#x27;/etc). TxtReaderActivity
// has no tag-aware rendering, so this collapses that markup down to plain
// text instead of leaving visible <tags> and &entities; on screen.
std::string htmlToPlainText(const std::string& input) {
  std::string noTags;
  noTags.reserve(input.size());
  bool inTag = false;
  for (size_t i = 0; i < input.size(); i++) {
    char c = input[i];
    if (c == '<') {
      // Paragraph/line breaks become a blank-line separator before the tag
      // itself is dropped, so multi-paragraph comments don't run together.
      if (input.compare(i, 3, "<p>") == 0 || input.compare(i, 3, "<p ") == 0 ||
          input.compare(i, 4, "<br>") == 0 || input.compare(i, 5, "<br/>") == 0) {
        noTags += "\n\n";
      }
      inTag = true;
      continue;
    }
    if (c == '>') {
      inTag = false;
      continue;
    }
    if (!inTag) noTags += c;
  }

  std::string out;
  out.reserve(noTags.size());
  for (size_t i = 0; i < noTags.size();) {
    if (noTags[i] == '&') {
      size_t semi = noTags.find(';', i);
      if (semi != std::string::npos && semi - i <= 10) {
        std::string entity = noTags.substr(i, semi - i + 1);
        bool matched = true;
        if (entity == "&amp;") {
          out += '&';
        } else if (entity == "&lt;") {
          out += '<';
        } else if (entity == "&gt;") {
          out += '>';
        } else if (entity == "&quot;") {
          out += '"';
        } else if (entity == "&#x27;" || entity == "&#39;") {
          out += '\'';
        } else if (entity == "&#x2F;" || entity == "&#47;") {
          out += '/';
        } else if (entity == "&nbsp;") {
          out += ' ';
        } else {
          matched = false;
        }
        if (matched) {
          i = semi + 1;
          continue;
        }
      }
    }
    out += noTags[i];
    i++;
  }
  return out;
}

// Recursively writes each comment as an author line followed by its
// (indented) body, with indentation depth standing in for the source app's
// nested <blockquote> tags.
void writePlainTextComments(HalFile& out, JsonArray comments, int depth, int& count, bool& truncated) {
  for (JsonObject comment : comments) {
    if (truncated) return;
    if (count >= kMaxComments) {
      truncated = true;
      return;
    }
    std::string text = comment["text"] | "";
    if (text.empty()) continue;  // deleted/flagged comments carry no text
    count++;

    std::string author = comment["author"] | "?";
    std::string indent(static_cast<size_t>(depth) * 2, ' ');
    std::string plain = htmlToPlainText(text);

    std::string header = indent + author + ":\n";
    out.write(header.data(), header.size());

    std::string bodyIndent = indent + "  ";
    size_t lineStart = 0;
    while (lineStart <= plain.size()) {
      size_t nl = plain.find('\n', lineStart);
      std::string line =
          (nl == std::string::npos) ? plain.substr(lineStart) : plain.substr(lineStart, nl - lineStart);
      std::string outLine = bodyIndent + line + "\n";
      out.write(outLine.data(), outLine.size());
      if (nl == std::string::npos) break;
      lineStart = nl + 1;
    }
    out.write("\n", 1);

    JsonArray children = comment["children"];
    writePlainTextComments(out, children, depth + 1, count, truncated);
  }
}

}  // namespace

std::string HackerNewsActivity::cachePath(int tab) const {
  static const char* names[HN_TAB_COUNT] = {"top", "new", "ask", "show"};
  return std::string("/apps/hackernews/") + names[tab] + ".json";
}

std::string HackerNewsActivity::tmpPath(int tab) const { return cachePath(tab) + ".tmp"; }

std::string HackerNewsActivity::apiUrl(int tab) const {
  switch (static_cast<HNTab>(tab)) {
    case HNTab::Top:
      return "https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=30";
    case HNTab::New:
      return "https://hn.algolia.com/api/v1/search_by_date?tags=story&hitsPerPage=30";
    case HNTab::Ask:
      return "https://hn.algolia.com/api/v1/search_by_date?tags=ask_hn&hitsPerPage=30";
    case HNTab::Show:
      return "https://hn.algolia.com/api/v1/search_by_date?tags=show_hn&hitsPerPage=30";
  }
  return "";
}

void HackerNewsActivity::startFetch(int tab) {
  state = HNState::Loading;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/hackernews");

  // Free every resident tab's vector before the TLS handshake: mbedTLS needs
  // a contiguous ~32KB (16KB in + 16KB out) buffer on this PSRAM-less chip,
  // and multiple previously-visited tabs' story lists sitting in RAM at once
  // is enough to fragment that away. Non-active tabs just get marked
  // unloaded; the tab-switch handler in loop() lazily reloads them from
  // their own SD cache (or re-fetches) the next time the user tabs to one.
  for (int t = 0; t < HN_TAB_COUNT; t++) {
    if (t == tab) continue;
    if (!stories[t].empty()) {
      stories[t].clear();
      stories[t].shrink_to_fit();
    }
    loaded[t] = false;
  }
  stories[tab].clear();
  stories[tab].shrink_to_fit();
  refreshFailed[tab] = false;

  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this, tab](const ActivityResult& result) {
                              if (result.isCancelled) {
                                // startFetch() already cleared this tab's vector above;
                                // restore it from disk since the fetch never started.
                                loadCacheFromSd(tab);
                                if (!loaded[tab]) {
                                  errorMessage[tab] = tr(STR_HN_WIFI_REQUIRED);
                                } else {
                                  refreshFailed[tab] = true;
                                }
                                state = HNState::CategoryList;
                                requestUpdate();
                              } else {
                                doFetch(tab);
                              }
                            });
    return;
  }

  doFetch(tab);
}

void HackerNewsActivity::doFetch(int tab) {
  requestUpdateAndWait();  // paint the "Loading"/"Refreshing" state before the blocking calls below

  bool success = false;
  int retries = 3;
  while (retries-- > 0) {
    if (HttpDownloader::downloadToFile(apiUrl(tab), tmpPath(tab)) == HttpDownloader::OK) {
      success = true;
      break;
    }
    if (retries > 0) delay(1500);
  }

  if (success) {
    Storage.remove(cachePath(tab).c_str());
    Storage.rename(tmpPath(tab).c_str(), cachePath(tab).c_str());
  }

  // The vector for this tab was cleared before the fetch started, so the
  // reload must happen unconditionally — on failure this is the only way to
  // get the old (still-good, untouched-on-disk) data back.
  loadCacheFromSd(tab);
  if (!loaded[tab]) {
    if (errorMessage[tab].empty()) errorMessage[tab] = tr(STR_HN_NO_DATA);
  } else if (!success) {
    refreshFailed[tab] = true;
  }
  state = HNState::CategoryList;
  requestUpdate();
}

bool HackerNewsActivity::loadCacheFromSd(int tab) {
  HalFile file;
  if (!Storage.openFileForRead("HN", cachePath(tab).c_str(), file)) {
    return false;
  }
  parseAndStoreList(tab, file);
  return loaded[tab];
}

void HackerNewsActivity::parseAndStoreList(int tab, HalFile& file) {
  struct HalFileJsonReader {
    HalFile& f;
    int read() { return f.read(); }
    size_t readBytes(char* buffer, size_t length) {
      const int n = f.read(buffer, length);
      return n < 0 ? 0 : static_cast<size_t>(n);
    }
  } reader{file};

  JsonDocument filter;
  filter["hits"][0]["objectID"] = true;
  filter["hits"][0]["title"] = true;
  filter["hits"][0]["url"] = true;
  filter["hits"][0]["author"] = true;
  filter["hits"][0]["points"] = true;
  filter["hits"][0]["num_comments"] = true;
  filter["hits"][0]["created_at_i"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, reader, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(10));
  if (err) {
    LOG_ERR("HN", "JSON parse failed for tab %d: %s", tab, err.c_str());
    errorMessage[tab] = tr(STR_HN_NO_DATA);
    return;
  }

  std::vector<HNStory> newStories;
  JsonArray hits = doc["hits"];
  newStories.reserve(hits.size());
  for (JsonObject hit : hits) {
    HNStory story;
    story.id = hit["objectID"] | "";
    story.title = hit["title"] | "";
    story.url = hit["url"] | "";
    story.author = hit["author"] | "";
    story.points = hit["points"] | 0;
    story.numComments = hit["num_comments"] | 0;
    story.relativeTime = formatRelativeTime(hit["created_at_i"] | 0);
    if (story.id.empty() || story.title.empty()) continue;
    newStories.push_back(std::move(story));
  }

  if (newStories.empty()) {
    errorMessage[tab] = tr(STR_HN_NO_DATA);
    return;
  }

  // Top is the one tab meant to read as a points ranking; the Algolia API
  // only returns it in front-page relevance order. New/Ask/Show are already
  // requested sorted by date (search_by_date), which is the right order for
  // them, so leave those alone.
  if (tab == static_cast<int>(HNTab::Top)) {
    std::stable_sort(newStories.begin(), newStories.end(),
                     [](const HNStory& a, const HNStory& b) { return a.points > b.points; });
  }

  stories[tab] = std::move(newStories);
  selectedRow[tab] = 0;
  loaded[tab] = true;
  errorMessage[tab].clear();
}

const HNStory* HackerNewsActivity::selectedStory() const {
  int tab = static_cast<int>(currentTab);
  if (detailStoryIndex < 0 || detailStoryIndex >= static_cast<int>(stories[tab].size())) return nullptr;
  return &stories[tab][detailStoryIndex];
}

void HackerNewsActivity::openComments() {
  const HNStory* storyPtr = selectedStory();
  if (!storyPtr) return;

  std::string destPath = "/apps/hackernews/comments/" + storyPtr->id + ".txt";
  if (Storage.exists(destPath.c_str())) {
    pushCommentsReader(destPath);
    return;
  }

  pendingCommentsStoryId = storyPtr->id;
  pendingCommentsStoryTitle = storyPtr->title;
  fetchingComments = true;
  commentsFetchFailed = false;
  state = HNState::Loading;
  requestUpdate();
  ensureWifiThenFetchComments();
}

void HackerNewsActivity::ensureWifiThenFetchComments() {
  if (WiFi.status() == WL_CONNECTED) {
    doFetchComments();
    return;
  }

  WiFi.mode(WIFI_STA);
  startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                          [this](const ActivityResult& result) {
                            if (result.isCancelled) {
                              fetchingComments = false;
                              state = HNState::StoryDetail;
                              requestUpdate();
                            } else {
                              doFetchComments();
                            }
                          });
}

void HackerNewsActivity::doFetchComments() {
  requestUpdateAndWait();  // paint the "Loading" state before the blocking calls below

  std::string destPath = "/apps/hackernews/comments/" + pendingCommentsStoryId + ".txt";
  bool ok = fetchAndWriteComments(pendingCommentsStoryId, pendingCommentsStoryTitle, destPath);

  fetchingComments = false;
  commentsFetchFailed = !ok;
  state = HNState::StoryDetail;
  requestUpdate();
  if (ok) {
    pushCommentsReader(destPath);
  }
}

bool HackerNewsActivity::fetchAndWriteComments(const std::string& storyId, const std::string& storyTitle,
                                               const std::string& destPath) const {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/hackernews");
  Storage.ensureDirectoryExists("/apps/hackernews/comments");

  std::string tmpJsonPath = "/apps/hackernews/comments_tmp.json";
  std::string url = "https://hn.algolia.com/api/v1/items/" + storyId;

  bool downloadOk = false;
  int retries = 3;
  while (retries-- > 0) {
    if (HttpDownloader::downloadToFile(url, tmpJsonPath) == HttpDownloader::OK) {
      downloadOk = true;
      break;
    }
    if (retries > 0) delay(1500);
  }

  bool ok = false;
  if (downloadOk) {
    HalFile jsonFile;
    if (Storage.openFileForRead("HN", tmpJsonPath.c_str(), jsonFile)) {
      struct HalFileJsonReader {
        HalFile& f;
        int read() { return f.read(); }
        size_t readBytes(char* buffer, size_t length) {
          const int n = f.read(buffer, length);
          return n < 0 ? 0 : static_cast<size_t>(n);
        }
      } reader{jsonFile};

      JsonDocument filter;
      buildCommentFilter(filter);
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter),
                                                 DeserializationOption::NestingLimit(kMaxCommentDepth + 4));
      jsonFile.close();
      if (!err) {
        HalFile outFile;
        if (Storage.openFileForWrite("HN", destPath.c_str(), outFile)) {
          std::string title = doc["title"] | storyTitle;
          std::string header = title + "\n" + std::string(title.size(), '=') + "\n\n";
          outFile.write(header.data(), header.size());

          std::string selfText = doc["text"] | "";
          if (!selfText.empty()) {
            std::string plain = htmlToPlainText(selfText) + "\n\n";
            outFile.write(plain.data(), plain.size());
          }

          JsonArray topLevel = doc["children"];
          if (topLevel.size() == 0) {
            std::string none = std::string(tr(STR_HN_NO_COMMENTS)) + "\n";
            outFile.write(none.data(), none.size());
          } else {
            int count = 0;
            bool truncated = false;
            writePlainTextComments(outFile, topLevel, 0, count, truncated);
            if (truncated) {
              std::string note = std::string(tr(STR_HN_COMMENTS_TRUNCATED)) + "\n";
              outFile.write(note.data(), note.size());
            }
          }
          outFile.close();
          ok = true;
        }
      } else {
        LOG_ERR("HN", "Comments JSON parse failed for %s: %s", storyId.c_str(), err.c_str());
      }
    }
  }

  Storage.remove(tmpJsonPath.c_str());
  return ok;
}

void HackerNewsActivity::pushCommentsReader(const std::string& path) {
  auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
  if (txt && txt->load()) {
    activityManager.pushActivity(makeUniqueNoThrow<TxtReaderActivity>(renderer, mappedInput, std::move(txt), 0));
  }
}

void HackerNewsActivity::showQrForLink() {
  const HNStory* storyPtr = selectedStory();
  if (!storyPtr || storyPtr->url.empty()) return;
  std::string url = storyPtr->url;
  startActivityForResult(makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, url),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void HackerNewsActivity::onEnter() {
  Activity::onEnter();
  int tab = static_cast<int>(currentTab);
  if (!loadCacheFromSd(tab)) {
    startFetch(tab);
  }
  requestUpdate();
}

void HackerNewsActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == HNState::Loading) {
    // A synchronous fetch (startFetch/doFetch or the comments equivalent) owns
    // whichever loop() call is currently blocked inside it; nothing to poll here.
    return;
  }

  if (state == HNState::StoryDetail) {
    if (mappedInput.wasReleased(Button::Back)) {
      state = HNState::CategoryList;
      requestUpdate();
      return;
    }
    const HNStory* story = selectedStory();
    const bool hasLink = story && !story->url.empty();
    const int itemCount = hasLink ? 2 : 1;
    if (mappedInput.wasReleased(Button::Up)) {
      detailMenuIndex = (detailMenuIndex - 1 + itemCount) % itemCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      detailMenuIndex = (detailMenuIndex + 1) % itemCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (detailMenuIndex == 0) {
        openComments();
      } else if (detailMenuIndex == 1) {
        showQrForLink();
      }
    }
    return;
  }

  // CategoryList
  int tab = static_cast<int>(currentTab);
  const int totalRows = static_cast<int>(stories[tab].size()) + 1;  // row 0 = synthetic "Refresh"

  if (mappedInput.wasReleased(Button::Back)) {
    onGoHome(HomeMenuItem::APPS_MENU);
    return;
  } else if (mappedInput.wasReleased(Button::Left)) {
    currentTab = static_cast<HNTab>(ButtonNavigator::previousIndex(tab, HN_TAB_COUNT));
    tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      if (!loadCacheFromSd(tab)) startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Right)) {
    currentTab = static_cast<HNTab>(ButtonNavigator::nextIndex(tab, HN_TAB_COUNT));
    tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      if (!loadCacheFromSd(tab)) startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Up)) {
    selectedRow[tab] = (selectedRow[tab] - 1 + totalRows) % totalRows;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Down)) {
    selectedRow[tab] = (selectedRow[tab] + 1) % totalRows;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    if (selectedRow[tab] == 0) {
      startFetch(tab);
    } else {
      detailStoryIndex = selectedRow[tab] - 1;
      detailMenuIndex = 0;
      commentsFetchFailed = false;
      state = HNState::StoryDetail;
      requestUpdate();
    }
  }
}

void HackerNewsActivity::drawTabStrip(int y, int selectedTab) const {
  const auto pageWidth = renderer.getScreenWidth();
  const char* labels[HN_TAB_COUNT] = {tr(STR_HN_TAB_TOP), tr(STR_HN_TAB_NEW), tr(STR_HN_TAB_ASK), tr(STR_HN_TAB_SHOW)};
  const int tabW = (pageWidth - 40) / HN_TAB_COUNT;
  for (int i = 0; i < HN_TAB_COUNT; i++) {
    const bool active = (i == selectedTab);
    const int tx = 20 + i * tabW;
    renderer.drawRoundedRect(tx + 2, y, tabW - 4, 30, 1, 5, true);
    if (active) {
      renderer.fillRoundedRect(tx + 2, y, tabW - 4, 30, 5, Color::Black);
    }
    const auto truncated = renderer.truncatedText(SMALL_FONT_ID, labels[i], tabW - 8);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, truncated.c_str());
    renderer.drawText(SMALL_FONT_ID, tx + (tabW - textW) / 2, y + 7, truncated.c_str(), !active);
  }
}

void HackerNewsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == HNState::StoryDetail) {
    const HNStory* story = selectedStory();
    const std::string title = story ? story->title : "";
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

    const int menuTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int menuBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const bool hasLink = story && !story->url.empty();
    const int itemCount = hasLink ? 2 : 1;

    GUI.drawButtonMenu(
        renderer, Rect{0, menuTop, pageWidth, menuBottom - menuTop}, itemCount, detailMenuIndex,
        [](int index) -> std::string { return index == 0 ? tr(STR_HN_VIEW_COMMENTS) : tr(STR_HN_SEND_TO_PHONE); },
        [](int) { return UIIcon::HackerNews; });

    if (story) {
      char metaBuf[96];
      snprintf(metaBuf, sizeof(metaBuf), tr(STR_HN_STORY_META), story->points, story->author.c_str(),
               story->relativeTime.c_str());
      renderer.drawCenteredText(SMALL_FONT_ID, menuBottom - 20, metaBuf, true, EpdFontFamily::REGULAR);
    }

    if (commentsFetchFailed) {
      GUI.drawPopup(renderer, tr(STR_HN_DOWNLOAD_FAILED));
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == HNState::Loading) {
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HN_TITLE));

    const int tabBarY = metrics.topPadding + metrics.headerHeight;
    const int tab = static_cast<int>(currentTab);
    drawTabStrip(tabBarY + 20, tab);

    const int contentTop = tabBarY + 20 + 30 + metrics.verticalSpacing;
    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int textY = contentTop + (listBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY,
                              fetchingComments ? tr(STR_HN_DOWNLOADING)
                                                : (loaded[tab] ? tr(STR_HN_REFRESHING) : tr(STR_HN_LOADING)));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HN_TITLE));

    const int tabBarY = metrics.topPadding + metrics.headerHeight;
    const int tab = static_cast<int>(currentTab);
    drawTabStrip(tabBarY + 20, tab);

    const int contentTop = tabBarY + 20 + 30 + metrics.verticalSpacing;
    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (!loaded[tab]) {
      const int textY = contentTop + (listBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      const char* msg = !errorMessage[tab].empty() ? errorMessage[tab].c_str() : tr(STR_HN_LOADING);
      renderer.drawCenteredText(UI_12_FONT_ID, textY, msg);
    } else {
      const auto& tabStories = stories[tab];
      const int totalRows = static_cast<int>(tabStories.size()) + 1;
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, listBottom - contentTop}, totalRows, selectedRow[tab],
          [&tabStories](int i) -> std::string {
            if (i == 0) return tr(STR_HN_REFRESH);
            return tabStories[i - 1].title;
          },
          [&tabStories](int i) -> std::string {
            if (i == 0) return "";
            const auto& s = tabStories[i - 1];
            char buf[96];
            snprintf(buf, sizeof(buf), tr(STR_HN_STORY_META), s.points, s.author.c_str(), s.relativeTime.c_str());
            return buf;
          },
          nullptr,
          [&tabStories](int i) -> std::string {
            if (i == 0) return "";
            char buf[16];
            snprintf(buf, sizeof(buf), tr(STR_HN_COMMENTS_VALUE), tabStories[i - 1].numComments);
            return buf;
          },
          true);
    }

    if (refreshFailed[tab]) {
      GUI.drawPopup(renderer, tr(STR_HN_REFRESH_FAILED));
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

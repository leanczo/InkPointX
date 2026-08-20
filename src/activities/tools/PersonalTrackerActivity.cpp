#include "PersonalTrackerActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

// Matches the URL's own limitToLast=20 - a redundant cap here so a feed
// shape change degrades to "20 rows" instead of an unbounded vector.
constexpr size_t kMaxReviews = 20;

// "videogame" -> "Videogame", "anime-movie" -> "Anime movie". Display-only
// polish; the underlying category string is never compared against this.
std::string categoryLabel(const std::string& category) {
  std::string result = category;
  for (char& c : result) {
    if (c == '-') c = ' ';
  }
  if (!result.empty()) result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
  return result;
}

// A line of already-wrapped detail text plus whether it should draw bold
// (a markdown heading, or the review's own title). Mirrors RssActivity's
// DetailLine, but scoped to this file: review content is hand-written
// Markdown (see MarkdownRenderer.tsx in lean-reviews), not RSS's HTML-laced
// feeds, so there's no tag-stripping or entity-decoding to do here.
struct ReviewDetailLine {
  std::string text;
  bool bold;
};

// A raw SOH byte marks the first character of a markdown heading line so it
// wraps in bold below - never occurs in real review text, and content is
// used immediately after this pass rather than round-tripped through any
// cache file, so there's no escaping concern.
constexpr char kHeadingSentinel = '\x01';

// "### Heading" -> its own paragraph, marked with kHeadingSentinel. Reviews
// only ever use the "# " ATX form at the start of a line (see
// MarkdownRenderer.tsx's parseBlocks), so no HTML heading tags to consider.
std::string promoteMarkdownHeaders(const std::string& raw) {
  std::string result;
  result.reserve(raw.length() + 16);
  size_t lineStart = 0;
  while (lineStart <= raw.length()) {
    size_t lineEnd = raw.find('\n', lineStart);
    const size_t contentEnd = (lineEnd == std::string::npos) ? raw.length() : lineEnd;

    size_t contentStart = lineStart;
    while (contentStart < contentEnd && (raw[contentStart] == ' ' || raw[contentStart] == '\t')) contentStart++;
    size_t hashCount = 0;
    while (hashCount < 6 && contentStart + hashCount < contentEnd && raw[contentStart + hashCount] == '#') {
      hashCount++;
    }
    const bool isHeader = hashCount > 0 && contentStart + hashCount < contentEnd &&
                          raw[contentStart + hashCount] == ' ';

    if (isHeader) {
      result += "\n\n";
      result += kHeadingSentinel;
      result.append(raw, contentStart + hashCount + 1, contentEnd - (contentStart + hashCount + 1));
      result += "\n\n";
    } else {
      result.append(raw, lineStart, contentEnd - lineStart);
    }

    if (lineEnd == std::string::npos) break;
    result += '\n';
    lineStart = lineEnd + 1;
  }
  return result;
}

// Strips paired ** bold markers only - single */_ italics are left as literal
// characters, same tradeoff RssActivity's stripMarkdownArtifacts makes (too
// many false positives in ordinary prose to safely pair them).
void stripBoldMarkers(std::string& text) {
  size_t pos = 0;
  while ((pos = text.find("**", pos)) != std::string::npos) {
    size_t close = text.find("**", pos + 2);
    if (close == std::string::npos) break;
    text.erase(close, 2);
    text.erase(pos, 2);
  }
}

// wrappedText() wraps by width only and has no concept of '\n' as a line
// break. This splits on the paragraph breaks promoteMarkdownHeaders() leaves
// behind and wraps each paragraph independently - same approach as
// RssActivity's wrapParagraphs().
std::vector<ReviewDetailLine> wrapReviewParagraphs(const GfxRenderer& renderer, int fontId, const std::string& text,
                                                    int maxWidth, int maxLines) {
  std::vector<ReviewDetailLine> allLines;
  size_t start = 0;
  while (start <= text.length() && static_cast<int>(allLines.size()) < maxLines) {
    size_t nl = text.find('\n', start);
    std::string paragraph = (nl == std::string::npos) ? text.substr(start) : text.substr(start, nl - start);

    const bool isHeading = !paragraph.empty() && paragraph.front() == kHeadingSentinel;
    if (isHeading) paragraph.erase(0, 1);

    if (paragraph.empty()) {
      allLines.push_back({"", false});
    } else {
      int budget = maxLines - static_cast<int>(allLines.size());
      auto paragraphLines = renderer.wrappedText(fontId, paragraph.c_str(), maxWidth, budget,
                                                  isHeading ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      for (auto& line : paragraphLines) allLines.push_back({std::move(line), isHeading});
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return allLines;
}

}  // namespace

std::string PersonalTrackerActivity::cachePath() { return "/apps/tracker/reviews.json"; }

std::string PersonalTrackerActivity::tmpPath() { return "/apps/tracker/reviews.tmp.json"; }

// The database host is personal to whoever builds this firmware, so it's
// never hardcoded here - set it via LEAN_REVIEWS_DB_URL in
// platformio.local.ini (gitignored, see .skills/SKILL.md), e.g.:
//   build_flags =
//     ${env:default.build_flags}
//     -DLEAN_REVIEWS_DB_URL=\"https://your-project-default-rtdb.firebaseio.com\"
#ifndef LEAN_REVIEWS_DB_URL
#define LEAN_REVIEWS_DB_URL ""
#endif

std::string PersonalTrackerActivity::apiUrl() {
  // Public-read REST GET against the same Firebase Realtime Database the
  // lean-reviews web app writes to (see reviewsStore.ts) - no SDK, no auth,
  // just JSON over HTTPS. %22 is the literal '"' Firebase's orderBy query
  // param requires around the field name.
  return std::string(LEAN_REVIEWS_DB_URL) + "/reviews.json?orderBy=%22createdAt%22&limitToLast=20";
}

std::string PersonalTrackerActivity::detailTmpPath() { return "/apps/tracker/detail.tmp.json"; }

std::string PersonalTrackerActivity::detailApiUrl(const std::string& id) {
  // Same public-read Firebase host as apiUrl(), just addressed straight at
  // one review node instead of the /reviews list - Firebase push keys are
  // "-" plus alphanumerics, so `id` never needs URL-encoding here.
  return std::string(LEAN_REVIEWS_DB_URL) + "/reviews/" + id + ".json";
}

bool PersonalTrackerActivity::parseAndStore(const std::string& json) {
  reviews.clear();

  // Only keep the fields this screen renders - each review's `content` and
  // `summary` can be several KB of HTML that would otherwise sit parsed in
  // RAM for no benefit. "*" is ArduinoJson's wildcard for "any object key",
  // needed here because reviews are keyed by push-id, not by array index.
  JsonDocument filter;
  filter["*"]["title"] = true;
  filter["*"]["category"] = true;
  filter["*"]["createdAt"] = true;
  filter["*"]["rating"] = true;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("TRACKER", "JSON parse failed (%u bytes): %s", static_cast<unsigned>(json.size()), err.c_str());
    return false;
  }

  // An empty /reviews node comes back as the JSON literal `null`, which is a
  // legitimate "no reviews yet" state, not a parse failure.
  JsonVariant root = doc.as<JsonVariant>();
  if (root.isNull()) return true;

  reviews.reserve(kMaxReviews);
  for (JsonPair kv : root.as<JsonObject>()) {
    if (reviews.size() >= kMaxReviews) break;
    JsonObject item = kv.value();
    TrackedReview r;
    r.id = kv.key().c_str();
    r.title = item["title"] | "";
    r.category = item["category"] | "";
    r.createdAt = item["createdAt"] | "";
    r.rating = item["rating"] | 0;
    if (r.title.empty()) continue;
    reviews.push_back(std::move(r));
  }

  // Firebase's own ordering guarantee for orderBy+limitToLast doesn't extend
  // to how a JSON *object* response gets walked here, so sort explicitly
  // instead of trusting fetch order. ISO "YYYY-MM-DD" compares correctly as
  // a plain string.
  std::sort(reviews.begin(), reviews.end(),
            [](const TrackedReview& a, const TrackedReview& b) { return a.createdAt > b.createdAt; });
  return true;
}

bool PersonalTrackerActivity::parseDetailJson(const std::string& json, std::string& outSummary,
                                              std::string& outContent) {
  // Only summary/content are needed here - title/category/date/rating are
  // already resident from the list fetch (see parseAndStore()'s comment on
  // why those two fields are excluded there).
  JsonDocument filter;
  filter["summary"] = true;
  filter["content"] = true;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("TRACKER", "Detail JSON parse failed (%u bytes): %s", static_cast<unsigned>(json.size()), err.c_str());
    return false;
  }

  JsonVariant root = doc.as<JsonVariant>();
  if (root.isNull()) return false;
  outSummary = root["summary"] | "";
  outContent = root["content"] | "";
  return true;
}

void PersonalTrackerActivity::doFetchDetail(const std::string& id) {
  requestUpdateAndWait();  // paint the "Loading review..." state before the blocking call below
  wifiWasUsed = true;

  const auto result = HttpDownloader::downloadToFile(detailApiUrl(id), detailTmpPath());
  detailLoading = false;

  if (result == HttpDownloader::OK) {
    const String input = Storage.readFile(detailTmpPath().c_str());
    Storage.remove(detailTmpPath().c_str());
    if (input.length() == 0 || !parseDetailJson(std::string(input.c_str()), detailSummary, detailContent)) {
      detailError = tr(STR_TRACKER_DETAIL_ERROR);
    }
  } else {
    detailError = tr(STR_TRACKER_DETAIL_ERROR);
  }
  requestUpdate();
}

void PersonalTrackerActivity::openDetail(int index) {
  if (index < 0 || index >= static_cast<int>(reviews.size())) return;

  detailOpen = true;
  detailLoading = true;
  detailError.clear();
  detailSummary.clear();
  detailContent.clear();
  detailScrollOffset = 0;
  requestUpdate();

  const std::string id = reviews[index].id;
  if (id.empty()) {
    detailLoading = false;
    detailError = tr(STR_TRACKER_DETAIL_ERROR);
    requestUpdate();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this, id](const ActivityResult& result) {
                              if (result.isCancelled) {
                                detailLoading = false;
                                detailError = tr(STR_TRACKER_WIFI_REQUIRED);
                                requestUpdate();
                              } else {
                                doFetchDetail(id);
                              }
                            });
    return;
  }

  doFetchDetail(id);
}

bool PersonalTrackerActivity::loadCacheFromSd() {
  const String input = Storage.readFile(cachePath().c_str());
  if (input.length() == 0) return false;
  loaded = parseAndStore(std::string(input.c_str()));
  return loaded;
}

void PersonalTrackerActivity::onEnter() {
  Activity::onEnter();
  if (!loadCacheFromSd()) startFetch();
  requestUpdate();
}

void PersonalTrackerActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void PersonalTrackerActivity::startFetch() {
  refreshing = true;
  refreshFailed = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/tracker");
  reviews.clear();
  loaded = false;
  errorMessage.clear();
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this](const ActivityResult& result) {
                              if (result.isCancelled) {
                                refreshing = false;
                                loadCacheFromSd();
                                if (!loaded) {
                                  errorMessage = tr(STR_TRACKER_WIFI_REQUIRED);
                                } else {
                                  refreshFailed = true;
                                }
                                requestUpdate();
                              } else {
                                doFetch();
                              }
                            });
    return;
  }

  doFetch();
}

namespace {
// LOG_ERR/LOG_DBG compile to nothing in release builds (see Logging.h -
// ENABLE_SERIAL_LOG is undefined there), so they're silent exactly in the
// builds most likely to be flashed to a real device. A bounded, stack-only
// line written straight to SD survives that and needs no USB to read back -
// same fix FootballActivity::doFetch applied after hitting this in the field.
void writeDebugLine(const char* line) {
  HalFile debugFile = Storage.open("/apps/tracker/debug.log", O_WRITE | O_CREAT | O_APPEND);
  if (!debugFile) return;
  debugFile.write(line, strlen(line));
  debugFile.close();
}
}  // namespace

void PersonalTrackerActivity::doFetch() {
  requestUpdateAndWait();  // paint the "Refreshing..." state before the blocking call below
  wifiWasUsed = true;

  const auto result = HttpDownloader::downloadToFile(apiUrl(), tmpPath());
  refreshing = false;

  if (result == HttpDownloader::OK) {
    Storage.remove(cachePath().c_str());
    Storage.rename(tmpPath().c_str(), cachePath().c_str());
  } else {
    char debugLine[128];
    snprintf(debugLine, sizeof(debugLine), "[%lu] fetch result=%d heap free=%u largest=%u\n",
              static_cast<unsigned long>(millis()), static_cast<int>(result), (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMaxAllocHeap());
    writeDebugLine(debugLine);
  }

  // The list was cleared before the fetch started, so the reload must happen
  // unconditionally - on failure this is the only way to get the old
  // (still-good, untouched-on-disk) data back.
  loadCacheFromSd();
  if (!loaded) {
    if (result == HttpDownloader::OK) {
      // Download succeeded but the response wasn't reviews - either the SD
      // write failed, or Firebase returned something parseAndStore couldn't
      // read (e.g. a rules-rejection body). Distinct from a network failure.
      char debugLine[96];
      snprintf(debugLine, sizeof(debugLine), "[%lu] downloaded OK but parse/load failed\n",
                static_cast<unsigned long>(millis()));
      writeDebugLine(debugLine);
    }
    if (errorMessage.empty()) errorMessage = tr(STR_TRACKER_NO_DATA);
  } else if (result != HttpDownloader::OK) {
    refreshFailed = true;
  }
  requestUpdate();
}

void PersonalTrackerActivity::loop() {
  if (detailOpen) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      detailOpen = false;
      detailSummary.clear();
      detailContent.clear();
      requestUpdate();
      return;
    }
    if (detailLoading) return;  // owned by the blocking doFetchDetail() call that triggered it
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (detailScrollOffset > 0) {
        detailScrollOffset = std::max(0, detailScrollOffset - detailMaxLines);
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      // render() re-clamps this to the last valid page, so it's safe to
      // overshoot here - a full-page jump so the screen fully replaces
      // instead of shifting by a single row each press.
      detailScrollOffset += detailMaxLines;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::APPS_MENU);
    return;
  }
  if (!reviews.empty() && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    selectedRow = (selectedRow - 1 + static_cast<int>(reviews.size())) % static_cast<int>(reviews.size());
    requestUpdate();
  } else if (!reviews.empty() && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selectedRow = (selectedRow + 1) % static_cast<int>(reviews.size());
    requestUpdate();
  } else if (!reviews.empty() && (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                                   mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    openDetail(selectedRow);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    startFetch();
  }
}

void PersonalTrackerActivity::render(RenderLock&& lock) {
  if (detailOpen) {
    renderer.clearScreen();

    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();
    const auto& metrics = UITheme::getInstance().getMetrics();

    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TRACKER_TITLE));

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int contentHeight = contentBottom - contentTop;

    const bool validIndex = selectedRow >= 0 && selectedRow < static_cast<int>(reviews.size());
    if (!validIndex) {
      // Stale selection (list shrunk from underneath this state) - bail back
      // to the list instead of an out-of-bounds vector access.
      detailOpen = false;
    } else if (detailLoading) {
      const int textY = contentTop + contentHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_TRACKER_DETAIL_LOADING));
    } else if (!detailError.empty()) {
      const int errWidth = pageWidth - 2 * metrics.contentSidePadding;
      auto errLines = renderer.wrappedText(UI_12_FONT_ID, detailError.c_str(), errWidth, 3, EpdFontFamily::REGULAR);
      const int errLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      int errY = contentTop + contentHeight / 2 - (static_cast<int>(errLines.size()) * errLineHeight) / 2;
      for (const auto& line : errLines) {
        renderer.drawCenteredText(UI_12_FONT_ID, errY, line.c_str());
        errY += errLineHeight;
      }
    } else {
      const auto& review = reviews[selectedRow];
      const int fontId = NOTOSANS_14_FONT_ID;
      const int lineHeight = renderer.getLineHeight(fontId);
      const int wrapWidth = pageWidth - 2 * metrics.contentSidePadding;

      // Title + meta line (bold/regular) followed by a blank spacer and the
      // review body, as one scrollable list - same shape as RssActivity's
      // PostDetail so Up/Down page through everything together.
      std::vector<ReviewDetailLine> lines;
      auto titleLines = renderer.wrappedText(fontId, review.title.c_str(), wrapWidth, 500, EpdFontFamily::BOLD);
      for (auto& l : titleLines) lines.push_back({l, true});

      char ratingBuf[8];
      snprintf(ratingBuf, sizeof(ratingBuf), "%d/10", review.rating);
      const std::string metaText =
          categoryLabel(review.category) + "  \xC2\xB7  " + review.createdAt + "  \xC2\xB7  " + ratingBuf;
      auto metaLines = renderer.wrappedText(fontId, metaText.c_str(), wrapWidth, 2, EpdFontFamily::REGULAR);
      for (auto& l : metaLines) lines.push_back({l, false});
      lines.push_back({"", false});

      std::string fullText = detailContent.empty() ? detailSummary : detailContent;
      if (fullText.empty()) {
        lines.push_back({tr(STR_TRACKER_DETAIL_EMPTY), false});
      } else {
        fullText = promoteMarkdownHeaders(fullText);
        stripBoldMarkers(fullText);
        auto bodyLines = wrapReviewParagraphs(renderer, fontId, fullText, wrapWidth, 1000);
        for (auto& l : bodyLines) lines.push_back(std::move(l));
      }

      int maxLines = std::max(1, contentHeight / lineHeight);
      detailMaxLines = maxLines;
      if (detailScrollOffset > std::max(0, static_cast<int>(lines.size()) - maxLines)) {
        detailScrollOffset = std::max(0, static_cast<int>(lines.size()) - maxLines);
      }

      for (int i = 0; i < maxLines; i++) {
        int lineIdx = detailScrollOffset + i;
        if (lineIdx >= static_cast<int>(lines.size())) break;
        const auto& line = lines[lineIdx];
        renderer.drawText(fontId, metrics.contentSidePadding, contentTop + i * lineHeight, line.text.c_str(), true,
                          line.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TRACKER_TITLE));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect listRect{0, listTop, pageWidth, listBottom - listTop};

  if (refreshing) {
    const int textY = listTop + (listBottom - listTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_TRACKER_REFRESHING));
  } else if (!loaded) {
    const char* msg = !errorMessage.empty() ? errorMessage.c_str() : tr(STR_TRACKER_LOADING);
    // drawCenteredText is single-line only; wrap long error text instead of
    // letting it overflow the screen edge (same fix as Football's/Sismos's).
    const int errWidth = pageWidth - 2 * metrics.contentSidePadding;
    auto errLines = renderer.wrappedText(UI_12_FONT_ID, msg, errWidth, 2, EpdFontFamily::REGULAR);
    const int errLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    int errY = listTop + (listBottom - listTop) / 2 - (static_cast<int>(errLines.size()) * errLineHeight) / 2;
    for (const auto& line : errLines) {
      renderer.drawCenteredText(UI_12_FONT_ID, errY, line.c_str());
      errY += errLineHeight;
    }
  } else if (reviews.empty()) {
    const int textY = listTop + (listBottom - listTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_TRACKER_EMPTY));
  } else {
    GUI.drawList(
        renderer, listRect, static_cast<int>(reviews.size()), selectedRow,
        [this](int i) { return reviews[i].title; },
        [this](int i) { return categoryLabel(reviews[i].category) + "  \xC2\xB7  " + reviews[i].createdAt; }, nullptr,
        [this](int i) {
          char buf[8];
          snprintf(buf, sizeof(buf), "%d/10", reviews[i].rating);
          return std::string(buf);
        },
        true);
  }

  if (refreshFailed) {
    GUI.drawPopup(renderer, tr(STR_TRACKER_REFRESH_FAILED));
  }

  const char* detailsLabel = reviews.empty() ? nullptr : tr(STR_TRACKER_DETAILS);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), detailsLabel, detailsLabel, tr(STR_TRACKER_REFRESH));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

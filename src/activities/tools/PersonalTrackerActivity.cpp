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
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    startFetch();
  }
}

void PersonalTrackerActivity::render(RenderLock&& lock) {
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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, tr(STR_TRACKER_REFRESH));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

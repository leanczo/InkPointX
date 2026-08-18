#include "OnThisDayActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/settings/ClockSyncActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"
#include "util/HoldGestures.h"

namespace {

const char* categoryKey(int category) {
  static const char* names[OTD_CATEGORY_COUNT] = {"events", "births", "deaths"};
  return names[category];
}

// This project only ships 5 UI languages (see lib/I18n/I18nKeys.h); anything
// else falls back to "en" the same way the doFetch language-fallback probe
// below would catch a wrong guess at runtime anyway.
const char* wikiLangCode(Language lang) {
  switch (lang) {
    case Language::ES:
      return "es";
    case Language::HE:
      return "he";
    case Language::AR:
      return "ar";
    case Language::KO:
      return "ko";
    default:
      return "en";
  }
}

}  // namespace

std::string OnThisDayActivity::cachePath(int category) const {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d-%02d", viewedDate.month, viewedDate.day);
  return "/apps/onthisday/" + activeLangCode + "_" + buf + "_" + categoryKey(category) + ".json";
}

std::string OnThisDayActivity::tmpPath(int category) const { return cachePath(category) + ".tmp"; }

std::string OnThisDayActivity::apiUrl(int category) const {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d/%02d", viewedDate.month, viewedDate.day);
  return "https://" + activeLangCode + ".wikipedia.org/api/rest_v1/feed/onthisday/" + categoryKey(category) + "/" +
        buf;
}

void OnThisDayActivity::startFetch(int category) {
  state = OnThisDayState::Loading;
  refreshing[category] = true;
  refreshFailed[category] = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/onthisday");

  // Free every resident category's vector before the TLS handshake: mbedTLS
  // needs a contiguous ~32KB (16KB in + 16KB out) buffer on this PSRAM-less
  // chip, and any category's list sitting in RAM is enough to fragment that
  // away. The other two categories just get marked unloaded; the tab-switch
  // guard in loop() lazily reloads them from their own SD cache (or
  // re-fetches) the next time the user tabs back to one.
  for (int c = 0; c < OTD_CATEGORY_COUNT; c++) {
    if (c == category) continue;
    if (!entries[c].empty()) {
      entries[c].clear();
      entries[c].shrink_to_fit();
    }
    loaded[c] = false;
  }
  entries[category].clear();
  entries[category].shrink_to_fit();
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this, category](const ActivityResult& result) {
                              if (result.isCancelled) {
                                refreshing[category] = false;
                                // startFetch() already cleared this category's vector
                                // above; restore it from disk since the fetch never started.
                                loadCacheFromSd(category);
                                if (!loaded[category]) {
                                  errorMessage[category] = tr(STR_OTD_WIFI_REQUIRED);
                                } else {
                                  refreshFailed[category] = true;
                                }
                                state = OnThisDayState::List;
                                requestUpdate();
                              } else {
                                doFetch(category);
                              }
                            });
    return;
  }

  doFetch(category);
}

void OnThisDayActivity::doFetch(int category) {
  requestUpdateAndWait();  // paint the "Loading"/"Refreshing" state before the blocking calls below
  wifiWasUsed = true;

  bool success = false;

  // One cheap, no-retry probe against the current language, run once per
  // activity lifetime. A 404 means this language subdomain doesn't carry
  // onthisday data at all -- a permanent condition, not a transient network
  // blip -- so it must not consume the 3x retry budget below, which exists
  // for flaky connections against a language that *is* supported.
  if (!langFallbackProbed) {
    langFallbackProbed = true;
    if (HttpDownloader::downloadToFile(apiUrl(category), tmpPath(category)) == HttpDownloader::OK) {
      success = true;
    } else if (activeLangCode != "en") {
      activeLangCode = "en";
    }
  }

  int retries = success ? 0 : 3;
  while (!success && retries-- > 0) {
    if (HttpDownloader::downloadToFile(apiUrl(category), tmpPath(category)) == HttpDownloader::OK) {
      success = true;
      break;
    }
    if (retries > 0) delay(1000);
  }

  refreshing[category] = false;
  if (success) {
    Storage.remove(cachePath(category).c_str());
    Storage.rename(tmpPath(category).c_str(), cachePath(category).c_str());
  }

  // The vector for this category was cleared before the fetch started, so
  // the reload must happen unconditionally — on failure this is the only way
  // to get the old (still-good, untouched-on-disk) data back.
  loadCacheFromSd(category);
  if (!loaded[category]) {
    if (errorMessage[category].empty()) errorMessage[category] = tr(STR_OTD_NO_DATA);
  } else if (!success) {
    refreshFailed[category] = true;
  }
  state = OnThisDayState::List;
  requestUpdate();
}

bool OnThisDayActivity::loadCacheFromSd(int category) {
  HalFile file;
  if (!Storage.openFileForRead("OTD", cachePath(category).c_str(), file)) {
    return false;
  }
  parseAndStore(category, file);
  return loaded[category];
}

void OnThisDayActivity::parseAndStore(int category, HalFile& file) {
  // Feed the open file straight to ArduinoJson so the ~700-800KB raw
  // response streams from SD a chunk at a time; only the small *filtered*
  // result tree is kept in RAM. Same pattern as FootballActivity::parseAndStore.
  struct HalFileJsonReader {
    HalFile& f;
    int read() { return f.read(); }
    size_t readBytes(char* buffer, size_t length) {
      const int n = f.read(buffer, length);
      return n < 0 ? 0 : static_cast<size_t>(n);
    }
  } reader{file};

  const char* key = categoryKey(category);
  JsonDocument filter;
  filter[key][0]["text"] = true;
  filter[key][0]["year"] = true;
  filter[key][0]["pages"][0]["content_urls"]["desktop"]["page"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, reader, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(10));
  if (err) {
    LOG_ERR("OTD", "JSON parse failed for category %d: %s", category, err.c_str());
    errorMessage[category] = tr(STR_OTD_NO_DATA);
    return;
  }

  // MAX_RESULT_ROWS only bounds the copy below -- the filtered JsonDocument
  // above still holds every filtered entry from the raw response. Known
  // limitation, same shape as Football's existing MAX_RESULT_ROWS cap.
  constexpr size_t MAX_RESULT_ROWS = 25;
  std::vector<OnThisDayEntry> newEntries;
  JsonArray items = doc[key];
  newEntries.reserve(std::min(items.size(), MAX_RESULT_ROWS));
  for (JsonObject item : items) {
    if (newEntries.size() >= MAX_RESULT_ROWS) break;
    std::string text = item["text"] | "";
    if (text.empty()) continue;
    OnThisDayEntry e;
    e.year = item["year"] | 0;
    e.text = std::move(text);
    JsonArray pages = item["pages"];
    if (pages.size() > 0) {
      JsonObject p0 = pages[0];
      e.pageUrl = p0["content_urls"]["desktop"]["page"] | "";
    }
    newEntries.push_back(std::move(e));
  }

  if (newEntries.empty()) {
    errorMessage[category] = tr(STR_OTD_NO_DATA);
    return;
  }

  entries[category] = std::move(newEntries);
  selectedRow[category] = 0;
  scrollOffset[category] = 0;
  loaded[category] = true;
  errorMessage[category].clear();
}

void OnThisDayActivity::changeDate(int deltaDays) {
  const int64_t days = DateMath::daysSinceEpoch(viewedDate) + deltaDays;
  viewedDate = DateMath::civilFromDays(days);  // year may now differ; never displayed/sent to the API
  reloadAllCategoriesForNewDate();
}

void OnThisDayActivity::reloadAllCategoriesForNewDate() {
  for (int c = 0; c < OTD_CATEGORY_COUNT; c++) {
    entries[c].clear();
    entries[c].shrink_to_fit();
    loaded[c] = false;
    errorMessage[c].clear();
    refreshFailed[c] = false;
    selectedRow[c] = 0;
    scrollOffset[c] = 0;
  }
  int cat = static_cast<int>(currentCategory);
  if (!loadCacheFromSd(cat)) startFetch(cat);
  requestUpdate();
}

void OnThisDayActivity::showQrForSelected() {
  int cat = static_cast<int>(currentCategory);
  if (detailEntryIndex < 0 || detailEntryIndex >= static_cast<int>(entries[cat].size())) return;
  const std::string url = entries[cat][detailEntryIndex].pageUrl;
  if (url.empty()) return;
  startActivityForResult(makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, url),
                         [this](const ActivityResult&) { requestUpdate(); });
}

std::string OnThisDayActivity::formattedHeaderDate() const {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d/%02d", viewedDate.day, viewedDate.month);
  return buf;
}

void OnThisDayActivity::onEnter() {
  Activity::onEnter();
  langFallbackProbed = false;
  activeLangCode = wikiLangCode(I18N.getLanguage());

  // Same clock read as CalendarActivity::getToday(): only trust the RTC once
  // it reports a valid sync, and fall back to a fixed date otherwise instead
  // of silently showing whatever the unsynced system clock happens to hold.
  struct tm t {};
  if (halClock.hasValidTime() && halClock.getDateTime(t, SETTINGS.clockUtcOffsetQ)) {
    viewedDate.year = static_cast<int16_t>(t.tm_year + 1900);
    viewedDate.month = static_cast<uint8_t>(t.tm_mon + 1);
    viewedDate.day = static_cast<uint8_t>(t.tm_mday);
  } else {
    viewedDate.year = 1970;
    viewedDate.month = 1;
    viewedDate.day = 1;
  }

  int cat = static_cast<int>(currentCategory);
  if (!loadCacheFromSd(cat)) {
    startFetch(cat);
  }
  requestUpdate();
}

void OnThisDayActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void OnThisDayActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == OnThisDayState::Loading) return;  // owned by the blocking startFetch()/doFetch() call that triggered it

  if (state == OnThisDayState::EntryDetail) {
    if (mappedInput.wasReleased(Button::Back)) {
      state = OnThisDayState::List;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(Button::Up)) {
      if (detailScrollOffset > 0) {
        detailScrollOffset = std::max(0, detailScrollOffset - detailMaxLines);
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(Button::Down)) {
      // render() re-clamps this to the last valid page, so it's safe to
      // overshoot here -- a full-page jump so the screen fully replaces
      // instead of shifting by a single line each press.
      detailScrollOffset += detailMaxLines;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      showQrForSelected();
    }
    return;
  }

  int cat = static_cast<int>(currentCategory);
  const int totalRows = 3 + static_cast<int>(entries[cat].size());  // 0=Refresh,1=PrevDay,2=NextDay

  // Long-press Back: force an NTP resync via ClockSyncActivity, same gesture
  // and entry point as ClockActivity::syncClock() / CalendarActivity. Not
  // gated on any "unconfirmed" flag -- a present-but-wrong time needs this
  // just as much as a missing one.
  if (mappedInput.isPressed(Button::Back) && mappedInput.getHeldTime() >= HoldGestures::LONG_MS) {
    startActivityForResult(makeUniqueNoThrow<ClockSyncActivity>(renderer, mappedInput),
                            [this](const ActivityResult&) {
                              struct tm t {};
                              if (halClock.hasValidTime() && halClock.getDateTime(t, SETTINGS.clockUtcOffsetQ)) {
                                viewedDate.year = static_cast<int16_t>(t.tm_year + 1900);
                                viewedDate.month = static_cast<uint8_t>(t.tm_mon + 1);
                                viewedDate.day = static_cast<uint8_t>(t.tm_mday);
                              }
                              reloadAllCategoriesForNewDate();
                            });
    return;
  }

  if (mappedInput.wasReleased(Button::Back)) {
    onGoHome(HomeMenuItem::APPS_MENU);
    return;
  } else if (mappedInput.wasReleased(Button::Left)) {
    currentCategory = static_cast<OnThisDayCategory>(ButtonNavigator::previousIndex(cat, OTD_CATEGORY_COUNT));
    cat = static_cast<int>(currentCategory);
    if (!loaded[cat] && errorMessage[cat].empty()) {
      if (!loadCacheFromSd(cat)) startFetch(cat);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Right)) {
    currentCategory = static_cast<OnThisDayCategory>(ButtonNavigator::nextIndex(cat, OTD_CATEGORY_COUNT));
    cat = static_cast<int>(currentCategory);
    if (!loaded[cat] && errorMessage[cat].empty()) {
      if (!loadCacheFromSd(cat)) startFetch(cat);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Up)) {
    if (selectedRow[cat] > 0) {
      selectedRow[cat]--;
      // Scrolling into view past the top is a simple snap, valid regardless
      // of card height; growing past the bottom depends on how many cards
      // actually fit at their natural (variable) height, which only
      // render() knows -- see its scroll-adjustment step.
      const int entryIdx = selectedRow[cat] - 3;
      if (entryIdx >= 0 && entryIdx < scrollOffset[cat]) scrollOffset[cat] = entryIdx;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(Button::Down)) {
    if (selectedRow[cat] < totalRows - 1) {
      selectedRow[cat]++;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    if (selectedRow[cat] == 0) {
      startFetch(cat);
    } else if (selectedRow[cat] == 1) {
      changeDate(-1);
    } else if (selectedRow[cat] == 2) {
      changeDate(1);
    } else {
      const int idx = selectedRow[cat] - 3;
      if (idx >= 0 && idx < static_cast<int>(entries[cat].size())) {
        detailEntryIndex = idx;
        detailScrollOffset = 0;
        state = OnThisDayState::EntryDetail;
        requestUpdate();
      }
    }
  }
}

void OnThisDayActivity::drawTabStrip(int y, int selectedTab) const {
  constexpr int kTabCount = OTD_CATEGORY_COUNT;
  constexpr int kTabHeight = 30;
  const auto pageWidth = renderer.getScreenWidth();
  const char* labels[kTabCount] = {tr(STR_OTD_TAB_EVENTS), tr(STR_OTD_TAB_BIRTHS), tr(STR_OTD_TAB_DEATHS)};
  const int tabW = (pageWidth - 40) / kTabCount;
  const int textH = renderer.getLineHeight(SMALL_FONT_ID);
  for (int i = 0; i < kTabCount; i++) {
    const bool active = (i == selectedTab);
    const int tx = 20 + i * tabW;
    renderer.drawRoundedRect(tx + 2, y, tabW - 4, kTabHeight, 1, 5, true);
    if (active) {
      renderer.fillRoundedRect(tx + 2, y, tabW - 4, kTabHeight, 5, Color::Black);
    }
    const auto truncated = renderer.truncatedText(SMALL_FONT_ID, labels[i], tabW - 8);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, truncated.c_str());
    renderer.drawText(SMALL_FONT_ID, tx + (tabW - textW) / 2, y + (kTabHeight - textH) / 2, truncated.c_str(), !active);
  }
}

void OnThisDayActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == OnThisDayState::EntryDetail) {
    const int cat = static_cast<int>(currentCategory);
    if (detailEntryIndex < 0 || detailEntryIndex >= static_cast<int>(entries[cat].size())) {
      // Stale index (list shrunk from underneath this state) -- bail back to
      // the list instead of an out-of-bounds vector access.
      state = OnThisDayState::List;
      renderer.displayBuffer();
      return;
    }
    const auto& e = entries[cat][detailEntryIndex];

    char yearBuf[16];
    if (e.year < 0) {
      snprintf(yearBuf, sizeof(yearBuf), tr(STR_OTD_YEAR_BC), -e.year);
    } else {
      snprintf(yearBuf, sizeof(yearBuf), "%d", e.year);
    }
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OTD_TITLE),
                  yearBuf);

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int sideX = metrics.contentSidePadding;
    const int wrapWidth = pageWidth - 2 * sideX;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

    // Uncapped wrap (maxLines=500, same convention as RssActivity's
    // PostDetail) -- the whole entry always ends up in `lines`, unlike the
    // list cards which cap at kMaxBodyLines and truncate with an ellipsis.
    auto lines = renderer.wrappedText(UI_12_FONT_ID, e.text.c_str(), wrapWidth, 500, EpdFontFamily::REGULAR);

    const int maxLines = std::max(1, (contentBottom - contentTop) / lineHeight);
    detailMaxLines = maxLines;
    const int maxOffset = std::max(0, static_cast<int>(lines.size()) - maxLines);
    if (detailScrollOffset > maxOffset) detailScrollOffset = maxOffset;

    int textY = contentTop;
    for (int i = 0; i < maxLines; i++) {
      const int lineIdx = detailScrollOffset + i;
      if (lineIdx >= static_cast<int>(lines.size())) break;
      renderer.drawText(UI_12_FONT_ID, sideX, textY, lines[lineIdx].c_str(), true, EpdFontFamily::REGULAR);
      textY += lineHeight;
    }

    const bool hasUrl = !e.pageUrl.empty();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), hasUrl ? tr(STR_OTD_SHOW_QR) : nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
    return;
  }

  const std::string dateStr = formattedHeaderDate();
  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OTD_TITLE),
                dateStr.c_str());

  const int tabBarY = metrics.topPadding + metrics.headerHeight;
  const int cat = static_cast<int>(currentCategory);
  drawTabStrip(tabBarY + 20, cat);

  const int contentTop = tabBarY + 20 + 30 + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (state == OnThisDayState::Loading) {
    const int textY = contentTop + (contentBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, loaded[cat] ? tr(STR_OTD_REFRESHING) : tr(STR_OTD_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int sideX = metrics.contentSidePadding;
  const int rowWidth = pageWidth - 2 * sideX;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // Refresh / Previous Day / Next Day as one row of compact buttons rather
  // than three stacked full-width rows -- same synthetic row indices (0,1,2)
  // and Up/Down/Confirm handling as before in loop(), just laid out
  // side-by-side so the row costs a third of the vertical space it used to.
  constexpr int kActionRowHeight = 30;
  constexpr int kActionGap = 8;
  const char* syntheticLabels[3] = {tr(STR_OTD_REFRESH), tr(STR_OTD_PREV_DAY), tr(STR_OTD_NEXT_DAY)};
  const int actionBtnW = (rowWidth - kActionGap * 2) / 3;
  const int actionTextH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = contentTop;
  for (int i = 0; i < 3; i++) {
    const bool isSelected = (selectedRow[cat] == i);
    const int bx = sideX + i * (actionBtnW + kActionGap);
    renderer.drawRoundedRect(bx, y, actionBtnW, kActionRowHeight, 1, 6, true);
    if (isSelected) renderer.fillRoundedRect(bx, y, actionBtnW, kActionRowHeight, 6, Color::Black);
    const auto label = renderer.truncatedText(UI_10_FONT_ID, syntheticLabels[i], actionBtnW - 8);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
    renderer.drawText(UI_10_FONT_ID, bx + (actionBtnW - textW) / 2, y + (kActionRowHeight - actionTextH) / 2,
                      label.c_str(), !isSelected);
  }
  y += kActionRowHeight + 6;

  if (entries[cat].empty()) {
    if (!errorMessage[cat].empty()) {
      const int wrapWidth = rowWidth - 40;  // margin so wrapped lines don't hug the screen edges
      auto errLines = renderer.wrappedText(UI_10_FONT_ID, errorMessage[cat].c_str(), wrapWidth, 4, EpdFontFamily::BOLD);
      int textY = y + 20;
      for (const auto& line : errLines) {
        renderer.drawCenteredText(UI_10_FONT_ID, textY, line.c_str(), true, EpdFontFamily::BOLD);
        textY += lineHeight;
      }
    }
  } else {
    // Cards are sized to their own wrapped-text line count (year row + up to
    // 3 body lines + padding) rather than an equal division of the content
    // area -- most entries wrap to 1-2 lines, so a fixed tall slot per card
    // wasted more than half the screen. Computing wrappedText per candidate
    // card is cheap: at most 25 entries exist, and e-ink only re-renders on
    // user action, not per frame.
    constexpr int kCardPadding = 18;  // top+bottom padding inside a card
    constexpr int kCardGap = 8;       // vertical gap between cards
    constexpr int kMaxBodyLines = 3;
    auto cardHeightFor = [&](const OnThisDayEntry& e) {
      auto lines = renderer.wrappedText(UI_10_FONT_ID, e.text.c_str(), rowWidth - 24, kMaxBodyLines, EpdFontFamily::REGULAR);
      return lineHeight + 4 + static_cast<int>(lines.size()) * lineHeight + kCardPadding + kCardGap;
    };

    // If the selected entry lies below the currently visible window, grow
    // scrollOffset until it fits; scrolling upward (selectedIdx < scrollOffset)
    // is already snapped by loop()'s Up handler.
    const int selectedEntryIdx = selectedRow[cat] - 3;
    if (selectedEntryIdx >= scrollOffset[cat] && selectedEntryIdx < static_cast<int>(entries[cat].size())) {
      while (true) {
        int yy = y;
        int lastVisible = scrollOffset[cat] - 1;
        for (int idx = scrollOffset[cat]; idx < static_cast<int>(entries[cat].size()); idx++) {
          const int h = cardHeightFor(entries[cat][idx]);
          if (yy + h > contentBottom) break;
          yy += h;
          lastVisible = idx;
        }
        if (selectedEntryIdx <= lastVisible || scrollOffset[cat] >= selectedEntryIdx) break;
        scrollOffset[cat]++;
      }
    }

    int drawY = y;
    for (int idx = scrollOffset[cat]; idx < static_cast<int>(entries[cat].size()); idx++) {
      const auto& e = entries[cat][idx];
      auto lines = renderer.wrappedText(UI_10_FONT_ID, e.text.c_str(), rowWidth - 24, kMaxBodyLines, EpdFontFamily::REGULAR);
      const int cardH = lineHeight + 4 + static_cast<int>(lines.size()) * lineHeight + kCardPadding + kCardGap;
      if (drawY + cardH > contentBottom) break;

      const bool isSelected = (selectedRow[cat] == idx + 3);
      renderer.drawRoundedRect(sideX, drawY, rowWidth, cardH - kCardGap, isSelected ? 3 : 1, 8, true);

      char yearBuf[16];
      if (e.year < 0) {
        snprintf(yearBuf, sizeof(yearBuf), tr(STR_OTD_YEAR_BC), -e.year);
      } else {
        snprintf(yearBuf, sizeof(yearBuf), "%d", e.year);
      }
      int textY = drawY + kCardPadding / 2;
      renderer.drawText(UI_10_FONT_ID, sideX + 12, textY, yearBuf, true, EpdFontFamily::BOLD);
      textY += lineHeight + 4;

      for (const auto& line : lines) {
        renderer.drawText(UI_10_FONT_ID, sideX + 12, textY, line.c_str(), true, EpdFontFamily::REGULAR);
        textY += lineHeight;
      }
      drawY += cardH;
    }
  }

  if (refreshFailed[cat]) {
    GUI.drawPopup(renderer, tr(STR_OTD_REFRESH_FAILED));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#include "HoroscopoActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

struct ZodiacSign {
  const char* apiSlug;  // freehoroscopeapi.com's English sign slug
  StrId labelKey;
};

// freehoroscopeapi.com has no Spanish endpoint (verified) -- the sign picker
// is localized, but doc["data"]["horoscope"] itself always comes back in
// English, same tradeoff already accepted for F1/Football data.
constexpr ZodiacSign kZodiacSigns[] = {
    {"aries", StrId::STR_ZODIAC_ARIES},         {"taurus", StrId::STR_ZODIAC_TAURUS},
    {"gemini", StrId::STR_ZODIAC_GEMINI},       {"cancer", StrId::STR_ZODIAC_CANCER},
    {"leo", StrId::STR_ZODIAC_LEO},             {"virgo", StrId::STR_ZODIAC_VIRGO},
    {"libra", StrId::STR_ZODIAC_LIBRA},         {"scorpio", StrId::STR_ZODIAC_SCORPIO},
    {"sagittarius", StrId::STR_ZODIAC_SAGITTARIUS}, {"capricorn", StrId::STR_ZODIAC_CAPRICORN},
    {"aquarius", StrId::STR_ZODIAC_AQUARIUS},   {"pisces", StrId::STR_ZODIAC_PISCES},
};
constexpr int kZodiacSignCount = sizeof(kZodiacSigns) / sizeof(kZodiacSigns[0]);

}  // namespace

void HoroscopoActivity::onEnter() {
  Activity::onEnter();
  state = HoroscopeState::SignSelect;
  selectedSignIndex = 0;
  loaded = false;
  refreshing = false;
  refreshFailed = false;
  wifiWasUsed = false;
  requestUpdate();
}

void HoroscopoActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void HoroscopoActivity::startFetch() {
  state = HoroscopeState::Loading;
  refreshing = true;
  refreshFailed = false;
  errorMessage.clear();
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this](const ActivityResult& result) {
                              refreshing = false;
                              if (result.isCancelled) {
                                if (!loaded) errorMessage = tr(STR_HOROSCOPE_WIFI_REQUIRED);
                                state = loaded ? HoroscopeState::Result : HoroscopeState::SignSelect;
                                requestUpdate();
                              } else {
                                doFetch();
                              }
                            });
    return;
  }

  doFetch();
}

void HoroscopoActivity::doFetch() {
  requestUpdateAndWait();  // paint the "Loading"/"Refreshing" state before the blocking call below
  wifiWasUsed = true;

  char url[160];
  snprintf(url, sizeof(url), "https://freehoroscopeapi.com/api/v1/get-horoscope/daily?sign=%s&day=today",
           kZodiacSigns[selectedSignIndex].apiSlug);

  std::string response;
  const bool ok = HttpDownloader::fetchUrl(url, response);
  refreshing = false;

  bool parsedOk = false;
  if (ok) {
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, response);
    if (!err) {
      const std::string text = doc["data"]["horoscope"] | "";
      if (!text.empty()) {
        horoscopeText = text;
        parsedOk = true;
      }
    }
  }

  if (parsedOk) {
    loaded = true;
    state = HoroscopeState::Result;
  } else if (!loaded) {
    errorMessage = tr(STR_HOROSCOPE_NO_DATA);
    state = HoroscopeState::SignSelect;
  } else {
    refreshFailed = true;
    state = HoroscopeState::Result;
  }
  requestUpdate();
}

void HoroscopoActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == HoroscopeState::Loading) return;  // owned by the blocking startFetch()/doFetch() call that triggered it

  if (mappedInput.wasReleased(Button::Back)) {
    if (state == HoroscopeState::Result) {
      loaded = false;
      state = HoroscopeState::SignSelect;
      requestUpdate();
    } else {
      onGoHome(HomeMenuItem::APPS_MENU);
    }
    return;
  }

  if (state == HoroscopeState::SignSelect) {
    if (mappedInput.wasReleased(Button::Up)) {
      selectedSignIndex = (selectedSignIndex - 1 + kZodiacSignCount) % kZodiacSignCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      selectedSignIndex = (selectedSignIndex + 1) % kZodiacSignCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      startFetch();
    }
    return;
  }

  if (state == HoroscopeState::Result) {
    if (mappedInput.wasReleased(Button::Right)) {
      startFetch();
    }
  }
}

void HoroscopoActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HOROSCOPE_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (state == HoroscopeState::SignSelect) {
    const Rect listRect{0, contentTop, pageWidth, contentBottom - contentTop};
    GUI.drawList(
        renderer, listRect, kZodiacSignCount, selectedSignIndex,
        [](int i) { return std::string(I18N.get(kZodiacSigns[i].labelKey)); }, nullptr, nullptr, nullptr, false);
    if (!errorMessage.empty()) {
      GUI.drawPopup(renderer, errorMessage.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == HoroscopeState::Loading) {
    const int textY = contentTop + (contentBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, loaded ? tr(STR_HOROSCOPE_REFRESHING) : tr(STR_HOROSCOPE_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {  // Result
    const std::string signLabel = I18N.get(kZodiacSigns[selectedSignIndex].labelKey);
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight}, signLabel.c_str());
    const int textTop = contentTop + metrics.subHeaderHeight + metrics.verticalSpacing;

    const int wrapWidth = pageWidth - 2 * metrics.contentSidePadding;
    auto lines = renderer.wrappedText(UI_10_FONT_ID, horoscopeText.c_str(), wrapWidth, 12, EpdFontFamily::REGULAR);
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    int textY = textTop;
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, textY, line.c_str(), true);
      textY += lineHeight;
    }

    if (refreshFailed) {
      GUI.drawPopup(renderer, tr(STR_HOROSCOPE_REFRESH_FAILED));
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, tr(STR_HOROSCOPE_REFRESH));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

#include "HoroscopoActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <WiFi.h>
#include <XmlParserUtils.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/HtmlTextCleanup.h"

namespace {

struct ZodiacSign {
  const char* apiSlug;  // horoscopo-del-dia.com's Spanish sign slug (matches its /horoscopo-diario-gratis/{slug}/ link)
  StrId labelKey;
};

constexpr ZodiacSign kZodiacSigns[] = {
    {"aries", StrId::STR_ZODIAC_ARIES},       {"tauro", StrId::STR_ZODIAC_TAURUS},
    {"geminis", StrId::STR_ZODIAC_GEMINI},    {"cancer", StrId::STR_ZODIAC_CANCER},
    {"leo", StrId::STR_ZODIAC_LEO},           {"virgo", StrId::STR_ZODIAC_VIRGO},
    {"libra", StrId::STR_ZODIAC_LIBRA},       {"escorpio", StrId::STR_ZODIAC_SCORPIO},
    {"sagitario", StrId::STR_ZODIAC_SAGITTARIUS}, {"capricornio", StrId::STR_ZODIAC_CAPRICORN},
    {"acuario", StrId::STR_ZODIAC_AQUARIUS},  {"piscis", StrId::STR_ZODIAC_PISCES},
};
constexpr int kZodiacSignCount = sizeof(kZodiacSigns) / sizeof(kZodiacSigns[0]);

constexpr StrId kLuckyColors[] = {
    StrId::STR_LUCKY_COLOR_RED,   StrId::STR_LUCKY_COLOR_ORANGE, StrId::STR_LUCKY_COLOR_YELLOW,
    StrId::STR_LUCKY_COLOR_GREEN, StrId::STR_LUCKY_COLOR_TURQUOISE, StrId::STR_LUCKY_COLOR_BLUE,
    StrId::STR_LUCKY_COLOR_PURPLE, StrId::STR_LUCKY_COLOR_PINK, StrId::STR_LUCKY_COLOR_GOLD,
    StrId::STR_LUCKY_COLOR_SILVER,
};
constexpr int kLuckyColorCount = sizeof(kLuckyColors) / sizeof(kLuckyColors[0]);

// No source publishes a "lucky number/color" over RSS (see the research in the
// PR this landed with -- every horoscope site just invents its own), so this
// derives both from a plain integer hash of the wall-clock date + sign index.
// Deterministic per day/sign, no state to persist, and exactly as legitimate
// as picking one out of a hat -- which is what the real sites do too.
uint32_t hashDateAndSign(int year, int month, int day, int signIndex) {
  uint32_t h = static_cast<uint32_t>(year) * 10000u + static_cast<uint32_t>(month) * 100u +
               static_cast<uint32_t>(day);
  h = h * 2654435761u + static_cast<uint32_t>(signIndex) * 40503u;
  h ^= h >> 15;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  return h;
}

// Streams horoscopo-del-dia.com's feed.xml (an RSS 2.0 feed with one <item>
// per zodiac sign) through Expat looking for the single <item> whose <link>
// contains the requested sign's slug, and captures its raw (HTML-in-CDATA)
// <description>. Everything after that item is still fed to Expat (cheaper
// than trying to abort mid-stream -- see doFetch()) but ignored immediately.
class HoroscopeItemParser {
 public:
  explicit HoroscopeItemParser(std::string linkFragment) : linkFragment(std::move(linkFragment)) {
    parser = XML_ParserCreate(nullptr);
    if (parser) {
      XML_SetUserData(parser, this);
      XML_SetElementHandler(parser, startElement, endElement);
      XML_SetCharacterDataHandler(parser, characterData);
    }
  }

  ~HoroscopeItemParser() { destroyXmlParser(parser); }

  bool feed(const char* data, int len) {
    if (!parser) return false;
    return XML_Parse(parser, data, len, XML_FALSE) != XML_STATUS_ERROR;
  }

  bool finish() {
    if (!parser) return false;
    return XML_Parse(parser, "", 0, XML_TRUE) != XML_STATUS_ERROR;
  }

  bool found() const { return itemFound; }
  const std::string& description() const { return foundDescription; }

 private:
  std::string linkFragment;
  XML_Parser parser = nullptr;
  bool inItem = false;
  bool itemFound = false;
  std::string currentTag;
  std::string currentText;
  std::string currentLink;
  std::string currentDescription;
  std::string foundDescription;

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<HoroscopeItemParser*>(userData);
    if (self->itemFound) return;
    std::string tag(name);
    for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (tag == "item") {
      self->inItem = true;
      self->currentLink.clear();
      self->currentDescription.clear();
    }
    if (self->inItem) {
      self->currentTag = tag;
      self->currentText.clear();
    }
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<HoroscopeItemParser*>(userData);
    if (self->itemFound) return;
    std::string tag(name);
    for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (tag == "item") {
      self->inItem = false;
      if (self->currentLink.find(self->linkFragment) != std::string::npos) {
        self->foundDescription = self->currentDescription;
        self->itemFound = true;
      }
    } else if (self->inItem) {
      if (tag == "link") {
        self->currentLink = self->currentText;
      } else if (tag == "description") {
        self->currentDescription = self->currentText;
      }
    }
    self->currentTag.clear();
    self->currentText.clear();
  }

  static void XMLCALL characterData(void* userData, const XML_Char* s, int len) {
    auto* self = static_cast<HoroscopeItemParser*>(userData);
    if (self->itemFound || !self->inItem) return;
    if (self->currentTag != "link" && self->currentTag != "description") return;
    const size_t limit = self->currentTag == "description" ? 4096 : 256;
    if (self->currentText.length() < limit) {
      const size_t toAppend = std::min(static_cast<size_t>(len), limit - self->currentText.length());
      self->currentText.append(s, toAppend);
    }
  }
};

}  // namespace

void HoroscopoActivity::onEnter() {
  Activity::onEnter();
  state = HoroscopeState::SignSelect;
  selectedSignIndex = std::min<int>(APP_STATE.lastHoroscopeSignIndex, kZodiacSignCount - 1);
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

  const std::string linkFragment =
      std::string("/horoscopo-diario-gratis/") + kZodiacSigns[selectedSignIndex].apiSlug + "/";
  HoroscopeItemParser parser(linkFragment);

  const bool ok = HttpDownloader::fetchUrl("https://horoscopo-del-dia.com/feed.xml",
                                            [&parser](const uint8_t* data, size_t len) {
                                              return parser.feed(reinterpret_cast<const char*>(data),
                                                                  static_cast<int>(len));
                                            });
  if (ok) parser.finish();
  refreshing = false;

  bool parsedOk = false;
  if (ok && parser.found()) {
    horoscopeText = HtmlTextCleanup::cleanField(parser.description());
    if (!horoscopeText.empty()) parsedOk = true;
  }

  if (parsedOk) {
    loaded = true;
    state = HoroscopeState::Result;
    detailScrollOffset = 0;
    updateLuckyLine();
  } else if (!loaded) {
    errorMessage = tr(STR_HOROSCOPE_NO_DATA);
    state = HoroscopeState::SignSelect;
  } else {
    refreshFailed = true;
    state = HoroscopeState::Result;
  }
  requestUpdate();
}

void HoroscopoActivity::updateLuckyLine() {
  struct tm t{};
  if (!halClock.hasValidTime() || !halClock.getDateTime(t, SETTINGS.clockUtcOffsetQ)) {
    luckyNumber = -1;
    luckyColorText.clear();
    return;
  }
  const uint32_t h = hashDateAndSign(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, selectedSignIndex);
  luckyNumber = 1 + static_cast<int>(h % 99u);
  luckyColorText = I18N.get(kLuckyColors[(h / 99u) % kLuckyColorCount]);
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
      if (APP_STATE.lastHoroscopeSignIndex != selectedSignIndex) {
        APP_STATE.lastHoroscopeSignIndex = static_cast<uint8_t>(selectedSignIndex);
        APP_STATE.saveToFile();
      }
      startFetch();
    }
    return;
  }

  if (state == HoroscopeState::Result) {
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
    } else if (mappedInput.wasReleased(Button::Right)) {
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
    char luckyBuf[64];
    if (luckyNumber >= 0) {
      snprintf(luckyBuf, sizeof(luckyBuf), tr(STR_HOROSCOPE_LUCKY_LINE), luckyNumber, luckyColorText.c_str());
    }
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight}, signLabel.c_str(),
                      luckyNumber >= 0 ? luckyBuf : nullptr);
    int textTop = contentTop + metrics.subHeaderHeight + metrics.verticalSpacing;

    const int wrapWidth = pageWidth - 2 * metrics.contentSidePadding;
    auto lines = HtmlTextCleanup::wrapParagraphs(renderer, UI_10_FONT_ID, horoscopeText, wrapWidth, 500,
                                                  EpdFontFamily::REGULAR);
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

    const int maxLines = std::max(1, (contentBottom - textTop) / lineHeight);
    detailMaxLines = maxLines;
    const int maxOffset = std::max(0, static_cast<int>(lines.size()) - maxLines);
    if (detailScrollOffset > maxOffset) detailScrollOffset = maxOffset;

    int textY = textTop;
    for (int i = 0; i < maxLines; i++) {
      const int lineIdx = detailScrollOffset + i;
      if (lineIdx >= static_cast<int>(lines.size())) break;
      const auto& line = lines[lineIdx];
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, textY, line.text.c_str(), true,
                        line.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
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

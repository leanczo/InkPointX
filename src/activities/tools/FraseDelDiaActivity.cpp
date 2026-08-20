#include "FraseDelDiaActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

struct FraseCategory {
  const char* jarName;  // wertarbyte.de/gigaset-rss "jar" query param
  StrId labelKey;
  int itemCount;  // entries in this jar, per the service's own form -- see below
};

// Clean (non-"off/*") Spanish fortune jars on wertarbyte.de/gigaset-rss,
// picked from that service's own form (verified live with curl -- each
// returns real content and a genuinely random entry per request). itemCount
// is a snapshot read from that same form's "jar (N)" labels -- it can only
// drift slowly (this is a static fortune-cookie database, not a live feed),
// and re-fetching it on every picker visit would mean a WiFi round-trip just
// to show a number, which the SCOPE.md on-demand/no-polling rule for these
// curated tools doesn't allow for something this cosmetic.
constexpr FraseCategory kFraseCategories[] = {
    {"refranes.fortunes", StrId::STR_FRASE_CAT_REFRANES, 4989},
    {"proverbios.fortunes", StrId::STR_FRASE_CAT_PROVERBIOS, 295},
    {"sabiduria.fortunes", StrId::STR_FRASE_CAT_SABIDURIA, 730},
    {"pintadas.fortunes", StrId::STR_FRASE_CAT_PINTADAS, 238},
    {"poder.fortunes", StrId::STR_FRASE_CAT_PODER, 337},
    {"sentimientos.fortunes", StrId::STR_FRASE_CAT_SENTIMIENTOS, 572},
    {"varios.fortunes", StrId::STR_FRASE_CAT_VARIOS, 597},
    {"verdad.fortunes", StrId::STR_FRASE_CAT_VERDAD, 153},
    {"schopenhauer.fortunes", StrId::STR_FRASE_CAT_SCHOPENHAUER, 13},
};
constexpr int kFraseCategoryCount = sizeof(kFraseCategories) / sizeof(kFraseCategories[0]);

// Calligraphy/font choices for the result screen, cycled with Left -- purely
// cosmetic (not persisted, resets to the script default every visit, same as
// the category selection) so a photo/screenshot can use a different look
// than daily reading. Mirrors RssActivity's kRssArticleFontIds pattern.
constexpr int kPhraseFontIds[] = {SCRIPT_FONT_ID, NOTOSERIF_18_FONT_ID, NOTOSANS_18_FONT_ID};
constexpr int kPhraseFontCount = sizeof(kPhraseFontIds) / sizeof(kPhraseFontIds[0]);

// Bounds how much of a single decoded entity/title this ever holds -- a
// misbehaving or unexpectedly large response from a third-party server must
// not grow these without limit (see heap-discipline: every accumulation
// needs a hard cap, not just a"should never happen" assumption).
constexpr size_t kMaxPhraseBytes = 2000;

// Decodes only what this specific feed actually emits: numeric character
// references (&#xHH; hex, &#DD; decimal -- how it escapes accented Spanish
// letters) plus the 5 standard named entities as a fallback. Simpler than
// RssActivity's decoder because there are no HTML tags to strip here, just
// escaped plain text.
std::string decodeEntities(const std::string& raw) {
  std::string out;
  out.reserve(raw.length());
  size_t i = 0;
  while (i < raw.length() && out.length() < kMaxPhraseBytes) {
    const char c = raw[i];
    if (c == '&') {
      const size_t semi = raw.find(';', i);
      if (semi != std::string::npos && semi - i <= 10) {
        const std::string entity = raw.substr(i + 1, semi - i - 1);
        long code = -1;
        if (!entity.empty() && entity[0] == '#') {
          if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
            code = strtol(entity.c_str() + 2, nullptr, 16);
          } else {
            code = strtol(entity.c_str() + 1, nullptr, 10);
          }
        } else if (entity == "amp") {
          code = '&';
        } else if (entity == "lt") {
          code = '<';
        } else if (entity == "gt") {
          code = '>';
        } else if (entity == "quot") {
          code = '"';
        } else if (entity == "apos") {
          code = '\'';
        }
        if (code > 0) {
          if (code <= 0x7F) {
            out += static_cast<char>(code);
          } else if (code <= 0x7FF) {
            out += static_cast<char>(0xC0 | ((code >> 6) & 0x1F));
            out += static_cast<char>(0x80 | (code & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | ((code >> 12) & 0x0F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
          }
          i = semi + 1;
          continue;
        }
      }
    }
    out += c;
    i++;
  }
  return out;
}

// The feed's channel element has its own <title>Gigaset Fortunes</title>
// before any <item> -- searching from the first <item> onward skips that and
// lands on the actual fortune. cookies=1 in the URL guarantees at most one
// <item>, so the first <title> found from there is the whole answer.
bool extractFortuneTitle(const std::string& xml, std::string& outText) {
  const size_t itemPos = xml.find("<item>");
  if (itemPos == std::string::npos) return false;  // category returned nothing
  const size_t titleStart = xml.find("<title>", itemPos);
  if (titleStart == std::string::npos) return false;
  const size_t textStart = titleStart + 7;  // strlen("<title>")
  const size_t titleEnd = xml.find("</title>", textStart);
  if (titleEnd == std::string::npos || titleEnd < textStart || titleEnd - textStart > kMaxPhraseBytes) return false;

  outText = decodeEntities(xml.substr(textStart, titleEnd - textStart));
  while (!outText.empty() && std::isspace(static_cast<unsigned char>(outText.back()))) outText.pop_back();
  size_t start = 0;
  while (start < outText.length() && std::isspace(static_cast<unsigned char>(outText[start]))) start++;
  outText.erase(0, start);
  return !outText.empty();
}

// Bounded, stack-only line appended to the SD card so a refresh failure can
// be diagnosed from /apps/frase/debug.log without a USB/serial connection --
// same pattern as FootballActivity/PersonalTrackerActivity's own debug.log.
// Logs every fetch (not just failures) so a working first fetch can be
// compared against a failing refresh, e.g. to see whether free heap/largest
// block dropped enough between the two for the TLS handshake to fail.
void appendDebugLog(const char* jarName, bool ok, bool parsedOk, size_t responseLen) {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/frase");
  char debugLine[160];
  const int len = snprintf(debugLine, sizeof(debugLine), "jar=%s ok=%d parsed=%d len=%u heap free=%u largest=%u\n",
                           jarName, static_cast<int>(ok), static_cast<int>(parsedOk),
                           static_cast<unsigned>(responseLen), (unsigned)ESP.getFreeHeap(),
                           (unsigned)ESP.getMaxAllocHeap());
  if (len <= 0) return;
  HalFile debugFile = Storage.open("/apps/frase/debug.log", O_WRITE | O_CREAT | O_APPEND);
  if (!debugFile) return;
  debugFile.write(debugLine, static_cast<size_t>(std::min(len, static_cast<int>(sizeof(debugLine)) - 1)));
  debugFile.close();
}

std::string apiUrl(const char* jarName) {
  return std::string("https://wertarbyte.de/gigaset-rss/?jar=") + jarName + "&lang=es&format=rss&limit=140&cookies=1";
}

}  // namespace

void FraseDelDiaActivity::onEnter() {
  Activity::onEnter();
  state = FraseState::CategoryPicker;
  selectedCategoryIndex = 0;
  fontChoiceIndex = 0;
  colorsInverted = false;
  loaded = false;
  refreshing = false;
  refreshFailed = false;
  colorsInverted = false;
  wifiWasUsed = false;
  requestUpdate();
}

void FraseDelDiaActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void FraseDelDiaActivity::startFetch() {
  state = FraseState::Loading;
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
                                if (!loaded) errorMessage = tr(STR_FRASE_WIFI_REQUIRED);
                                state = loaded ? FraseState::Result : FraseState::CategoryPicker;
                                requestUpdate();
                              } else {
                                doFetch();
                              }
                            });
    return;
  }

  doFetch();
}

void FraseDelDiaActivity::doFetch() {
  requestUpdateAndWait();  // paint the "Loading"/"Refreshing" state before the blocking call below
  wifiWasUsed = true;

  std::string response;
  // fetchUrl() (unlike HttpDownloader::downloadToFile()) makes a single
  // attempt with no TLS-heap-headroom check or retry. On this PSRAM-less C3
  // the TLS handshake is the largest transient allocation, and a fragmented
  // heap can reject one handshake even though every allocation from that
  // failed attempt is released right after (see HttpDownloader.cpp's runGet()
  // comment on this) -- a fresh attempt then has a clean arena and usually
  // succeeds. That's the "occasionally on refresh" failure users see; one
  // immediate retry covers it without the complexity of a full backoff loop.
  const std::string url = apiUrl(kFraseCategories[selectedCategoryIndex].jarName);
  bool ok = HttpDownloader::fetchUrl(url, response);
  if (!ok) ok = HttpDownloader::fetchUrl(url, response);
  refreshing = false;

  std::string text;
  const bool parsedOk = ok && extractFortuneTitle(response, text);
  appendDebugLog(kFraseCategories[selectedCategoryIndex].jarName, ok, parsedOk, response.size());

  if (parsedOk) {
    phraseText = text;
    loaded = true;
    state = FraseState::Result;
  } else if (!loaded) {
    errorMessage = tr(STR_FRASE_NO_DATA);
    state = FraseState::CategoryPicker;
  } else {
    refreshFailed = true;
    state = FraseState::Result;
  }
  requestUpdate();
}

void FraseDelDiaActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == FraseState::Loading) return;  // owned by the blocking startFetch()/doFetch() call that triggered it

  if (mappedInput.wasReleased(Button::Back)) {
    if (state == FraseState::Result) {
      loaded = false;
      colorsInverted = false;
      state = FraseState::CategoryPicker;
      requestUpdate();
    } else {
      onGoHome(HomeMenuItem::APPS_MENU);
    }
    return;
  }

  if (state == FraseState::CategoryPicker) {
    if (mappedInput.wasReleased(Button::Up)) {
      selectedCategoryIndex = (selectedCategoryIndex - 1 + kFraseCategoryCount) % kFraseCategoryCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      selectedCategoryIndex = (selectedCategoryIndex + 1) % kFraseCategoryCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      startFetch();
    }
    return;
  }

  if (state == FraseState::Result) {
    if (mappedInput.wasReleased(Button::Right) || mappedInput.wasReleased(Button::Confirm)) {
      startFetch();  // another random phrase from the same category
    } else if (mappedInput.wasReleased(Button::Left)) {
      fontChoiceIndex = (fontChoiceIndex + 1) % kPhraseFontCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      colorsInverted = !colorsInverted;
      requestUpdate();
    }
  }
}

void FraseDelDiaActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FRASE_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (state == FraseState::CategoryPicker) {
    const Rect listRect{0, contentTop, pageWidth, contentBottom - contentTop};
    GUI.drawList(
        renderer, listRect, kFraseCategoryCount, selectedCategoryIndex,
        [](int i) { return std::string(I18N.get(kFraseCategories[i].labelKey)); }, nullptr, nullptr,
        [](int i) { return std::to_string(kFraseCategories[i].itemCount); });
    if (!errorMessage.empty()) {
      GUI.drawPopup(renderer, errorMessage.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FraseState::Loading) {
    const int textY = contentTop + (contentBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, loaded ? tr(STR_FRASE_REFRESHING) : tr(STR_FRASE_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {  // Result
    // No subheader with the category name here -- the quote itself, defaulting
    // to the same handwritten script face as the screen titles instead of the
    // plain body sans (Left cycles kPhraseFontIds for a different look, e.g.
    // for a photo/screenshot), is the whole point of this screen; a
    // minimalist, single-focus layout suits that better than an extra
    // structural label.
    const int phraseFontId = kPhraseFontIds[fontChoiceIndex];
    const int wrapWidth = pageWidth - 2 * metrics.contentSidePadding - 20;
    auto lines = renderer.wrappedText(phraseFontId, phraseText.c_str(), wrapWidth, 10, EpdFontFamily::REGULAR);
    const int lineHeight = renderer.getLineHeight(phraseFontId);
    const int blockHeight = static_cast<int>(lines.size()) * lineHeight;
    int textY = contentTop + std::max(0, (contentBottom - contentTop - blockHeight) / 2);
    for (const auto& line : lines) {
      renderer.drawCenteredText(phraseFontId, textY, line.c_str());
      textY += lineHeight;
    }

    if (refreshFailed) {
      GUI.drawPopup(renderer, tr(STR_FRASE_REFRESH_FAILED));
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, tr(STR_FRASE_FONT), tr(STR_FRASE_REFRESH));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    if (colorsInverted) renderer.invertScreen();
  }

  // Same primitive the reader's night mode uses (SETTINGS.readerInvertColors
  // + renderer.invertScreen()) -- a bitwise NOT of the whole framebuffer after
  // every draw call above, so header and button hints invert along with the
  // quote. Down toggles it; kept local to this activity rather than the
  // persisted reader setting since it's for a one-off photo, not a standing
  // reading preference.
  if (state == FraseState::Result && colorsInverted) renderer.invertScreen();

  renderer.displayBuffer();
}

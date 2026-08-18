#include "SismosActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <XmlParserUtils.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

// INPRES's own "last 50" feed - matches its own page's window, so this list
// never grows past what a single fetch already returns.
constexpr size_t kMaxSismos = 50;

// INPRES sends provinces in plain-ASCII upper case ("SAN JUAN", "LA RIOJA");
// this only ever runs on that data, so a byte-wise ASCII toupper/tolower is
// enough - no accented letters appear in the feed to mishandle.
std::string toTitleCase(const std::string& text) {
  std::string result = text;
  bool startOfWord = true;
  for (char& c : result) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      startOfWord = true;
    } else {
      c = static_cast<char>(startOfWord ? std::toupper(static_cast<unsigned char>(c))
                                         : std::tolower(static_cast<unsigned char>(c)));
      startOfWord = false;
    }
  }
  return result;
}

// Streams the feed a chunk at a time through expat and fills `out` directly -
// at most 50 small structs, never the raw XML - same shape as RssActivity's
// RssParser, minus the markdown-file output stage RSS needs and this doesn't.
class SismosXmlParser {
 public:
  explicit SismosXmlParser(std::vector<Sismo>& out) : out(out) {
    parser = XML_ParserCreate(nullptr);
    if (parser) {
      XML_SetUserData(parser, this);
      XML_SetElementHandler(parser, startElement, endElement);
      XML_SetCharacterDataHandler(parser, characterData);
    }
  }
  ~SismosXmlParser() { destroyXmlParser(parser); }

  bool parseBuffer(const char* data, int len, bool isFinal) {
    if (!parser) return false;
    if (XML_Parse(parser, data, len, isFinal) == XML_STATUS_ERROR) {
      LOG_DBG("SISMOS", "Parse error: %s at line %lu", XML_ErrorString(XML_GetErrorCode(parser)),
              XML_GetCurrentLineNumber(parser));
      return false;
    }
    return true;
  }

 private:
  std::vector<Sismo>& out;
  XML_Parser parser = nullptr;
  bool inItem = false;
  std::string currentTag;
  std::string currentText;
  Sismo current;

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<SismosXmlParser*>(userData);
    const std::string tag(name);
    if (tag == "item") {
      self->inItem = true;
      self->current = Sismo();
    } else if (self->inItem) {
      self->currentTag = tag;
      self->currentText.clear();
    }
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<SismosXmlParser*>(userData);
    const std::string tag(name);
    if (tag == "item") {
      self->inItem = false;
      if (self->out.size() < kMaxSismos) self->out.push_back(self->current);
      // The feed itself already caps at 50, but stop the parse the moment we
      // have enough rather than trusting that - a feed shape change should
      // degrade to "50 rows" here, not to growing this vector unbounded.
      if (self->out.size() >= kMaxSismos) XML_StopParser(self->parser, XML_FALSE);
    } else if (self->inItem) {
      if (tag == "idSismo") {
        self->current.idSismo = self->currentText;
      } else if (tag == "fecha") {
        self->current.date = self->currentText;
      } else if (tag == "hora") {
        self->current.time = self->currentText;
      } else if (tag == "prof") {
        self->current.depthKm = atoi(self->currentText.c_str());
      } else if (tag == "mg") {
        self->current.magnitude = static_cast<float>(atof(self->currentText.c_str()));
      } else if (tag == "prov") {
        self->current.province = toTitleCase(self->currentText);
      } else if (tag == "color_link") {
        if (self->currentText == "f00") {
          self->current.reviewState = SismoReviewState::Felt;
        } else if (self->currentText == "000") {
          self->current.reviewState = SismoReviewState::Reviewed;
        } else {
          self->current.reviewState = SismoReviewState::Auto;
        }
      }
    }
    self->currentTag.clear();
    self->currentText.clear();
  }

  static void XMLCALL characterData(void* userData, const XML_Char* s, const int len) {
    auto* self = static_cast<SismosXmlParser*>(userData);
    if (self->inItem && !self->currentTag.empty() && self->currentText.length() < 64) {
      const size_t toAppend = std::min(static_cast<size_t>(len), size_t(64) - self->currentText.length());
      self->currentText.append(s, toAppend);
    }
  }
};

}  // namespace

std::string SismosActivity::cachePath() { return "/apps/sismos/sismos.xml"; }

std::string SismosActivity::tmpPath() { return "/apps/sismos/sismos.tmp.xml"; }

std::string SismosActivity::apiUrl() {
  // The public page at inpres.gob.ar/sismologia/xultimos populates its table
  // client-side from this same XML - fetching it directly skips the ~11KB of
  // surrounding page HTML this device would otherwise have to scan through
  // for no benefit.
  return "https://www.inpres.gob.ar/mapa/sismos.xml";
}

void SismosActivity::parseAndStore(HalFile& file) {
  sismos.clear();
  sismos.reserve(kMaxSismos);
  SismosXmlParser parser(sismos);
  char buffer[1024];
  while (file.available() > 0) {
    const int bytesRead = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
    if (bytesRead <= 0) break;
    if (!parser.parseBuffer(buffer, bytesRead, file.available() == 0)) break;
  }
  LOG_DBG("SISMOS", "Parsed %u sismo(s)", static_cast<unsigned>(sismos.size()));
}

bool SismosActivity::loadCacheFromSd() {
  HalFile file;
  if (!Storage.openFileForRead("SISMOS", cachePath().c_str(), file)) return false;
  parseAndStore(file);
  // An empty result here means the feed's shape changed under us or the
  // download was truncated - Argentina's "last 50" window is never actually
  // empty, so treat it as a failure rather than a legitimate empty state.
  loaded = !sismos.empty();
  return loaded;
}

void SismosActivity::onEnter() {
  Activity::onEnter();
  if (!loadCacheFromSd()) startFetch();
  requestUpdate();
}

void SismosActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void SismosActivity::startFetch() {
  refreshing = true;
  refreshFailed = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/sismos");
  sismos.clear();
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
                                  errorMessage = tr(STR_SISMOS_WIFI_REQUIRED);
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

void SismosActivity::doFetch() {
  requestUpdateAndWait();  // paint the "Refreshing..." state before the blocking call below
  wifiWasUsed = true;

  const auto result = HttpDownloader::downloadToFile(apiUrl(), tmpPath());
  refreshing = false;

  if (result == HttpDownloader::OK) {
    Storage.remove(cachePath().c_str());
    Storage.rename(tmpPath().c_str(), cachePath().c_str());
  }

  // The list was cleared before the fetch started, so the reload must happen
  // unconditionally - on failure this is the only way to get the old
  // (still-good, untouched-on-disk) data back.
  loadCacheFromSd();
  if (!loaded) {
    if (errorMessage.empty()) errorMessage = tr(STR_SISMOS_NO_DATA);
  } else if (result != HttpDownloader::OK) {
    refreshFailed = true;
  }
  requestUpdate();
}

void SismosActivity::showMapForSelected() {
  if (selectedRow < 0 || selectedRow >= static_cast<int>(sismos.size())) return;
  const std::string& id = sismos[selectedRow].idSismo;
  if (id.empty()) return;
  startActivityForResult(makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, "https://www.inpres.gob.ar/mapa/" + id),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void SismosActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::APPS_MENU);
    return;
  }
  if (!sismos.empty() && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    selectedRow = (selectedRow - 1 + static_cast<int>(sismos.size())) % static_cast<int>(sismos.size());
    requestUpdate();
  } else if (!sismos.empty() && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selectedRow = (selectedRow + 1) % static_cast<int>(sismos.size());
    requestUpdate();
  } else if (!sismos.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showMapForSelected();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    startFetch();
  }
}

void SismosActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SISMOS_TITLE));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect listRect{0, listTop, pageWidth, listBottom - listTop};

  if (refreshing) {
    const int textY = listTop + (listBottom - listTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_SISMOS_REFRESHING));
  } else if (!loaded) {
    const char* msg = !errorMessage.empty() ? errorMessage.c_str() : tr(STR_SISMOS_LOADING);
    // drawCenteredText is single-line only; wrap long error text instead of
    // letting it overflow the screen edge (same fix as Football's).
    const int errWidth = pageWidth - 2 * metrics.contentSidePadding;
    auto errLines = renderer.wrappedText(UI_12_FONT_ID, msg, errWidth, 2, EpdFontFamily::REGULAR);
    const int errLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    int errY = listTop + (listBottom - listTop) / 2 - (static_cast<int>(errLines.size()) * errLineHeight) / 2;
    for (const auto& line : errLines) {
      renderer.drawCenteredText(UI_12_FONT_ID, errY, line.c_str());
      errY += errLineHeight;
    }
  } else {
    GUI.drawList(
        renderer, listRect, static_cast<int>(sismos.size()), selectedRow,
        [this](int i) { return sismos[i].province; },
        [this](int i) { return sismos[i].date + "  " + sismos[i].time; }, nullptr,
        [this](int i) {
          char buf[24];
          snprintf(buf, sizeof(buf), "M%.1f \xC2\xB7 %dkm", sismos[i].magnitude, sismos[i].depthKm);
          return std::string(buf);
        },
        true, [this](int i) { return sismos[i].reviewState == SismoReviewState::Auto; });
  }

  if (refreshFailed) {
    GUI.drawPopup(renderer, tr(STR_SISMOS_REFRESH_FAILED));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SISMOS_VIEW_MAP), nullptr, tr(STR_SISMOS_REFRESH));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#include "DuckDuckGoActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

constexpr int kMaxRecentSearches = 10;

std::string urlEncode(const std::string& value) {
  std::string escaped;
  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped += c;
    } else if (c == ' ') {
      escaped += '+';
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
      escaped += hex;
    }
  }
  return escaped;
}

std::string urlDecode(const std::string& src) {
  std::string ret;
  for (size_t i = 0; i < src.length(); i++) {
    if (src[i] == '%' && i + 2 < src.length()) {
      int value = 0;
      sscanf(src.substr(i + 1, 2).c_str(), "%x", &value);
      ret += static_cast<char>(value);
      i += 2;
    } else if (src[i] == '+') {
      ret += ' ';
    } else {
      ret += src[i];
    }
  }
  return ret;
}

std::string decodeHtmlEntities(const std::string& input) {
  std::string output;
  output.reserve(input.length());
  for (size_t i = 0; i < input.length();) {
    if (input[i] == '&') {
      size_t semi = input.find(';', i);
      if (semi != std::string::npos && semi - i < 10) {
        std::string entity = input.substr(i + 1, semi - i - 1);
        bool matched = true;
        if (entity == "amp") {
          output += '&';
        } else if (entity == "quot") {
          output += '"';
        } else if (entity == "lt") {
          output += '<';
        } else if (entity == "gt") {
          output += '>';
        } else if (entity == "apos" || entity == "#x27" || entity == "#39") {
          output += '\'';
        } else if (entity == "nbsp") {
          output += ' ';
        } else {
          matched = false;
        }
        if (matched) {
          i = semi + 1;
          continue;
        }
      }
    }
    output += input[i];
    i++;
  }
  return output;
}

std::string cleanHtmlText(const std::string& input) {
  std::string decoded = decodeHtmlEntities(input);
  std::string output;
  bool lastWasSpace = true;
  for (char c : decoded) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!lastWasSpace) {
        output += ' ';
        lastWasSpace = true;
      }
    } else {
      output += c;
      lastWasSpace = false;
    }
  }
  if (!output.empty() && output.back() == ' ') output.pop_back();
  return output;
}

std::string extractHref(const std::string& attribs) {
  size_t pos = attribs.find("href");
  if (pos == std::string::npos) pos = attribs.find("HREF");
  if (pos == std::string::npos) return "";

  size_t eqPos = attribs.find('=', pos);
  if (eqPos == std::string::npos) return "";

  size_t valPos = eqPos + 1;
  while (valPos < attribs.length() && (attribs[valPos] == ' ' || attribs[valPos] == '\t')) valPos++;
  if (valPos >= attribs.length()) return "";

  char quote = attribs[valPos];
  if (quote == '"' || quote == '\'') {
    size_t endQuote = attribs.find(quote, valPos + 1);
    if (endQuote != std::string::npos) return attribs.substr(valPos + 1, endQuote - (valPos + 1));
    return attribs.substr(valPos + 1);
  }
  size_t space = attribs.find_first_of(" \t", valPos);
  if (space != std::string::npos) return attribs.substr(valPos, space - valPos);
  return attribs.substr(valPos);
}

// Hand-rolled streaming HTML scanner for DuckDuckGo's HTML-only search
// endpoint (html.duckduckgo.com): pulls the title text and href out of every
// <a>...</a>, resolving the "uddg=" redirect wrapper DuckDuckGo puts on
// result links back to the real target URL.
bool parseDuckDuckGoResults(const std::string& htmlPath, std::vector<DuckLink>& links) {
  HalFile file;
  if (!Storage.openFileForRead("DDG", htmlPath.c_str(), file)) return false;

  links.clear();

  enum class ParserState { Scanning, InTag, InTagAttribs, InAnchorContent };
  ParserState state = ParserState::Scanning;
  std::string currentTagName;
  std::string currentTagAttribs;
  std::string currentText;

  int c;
  while ((c = file.read()) != -1) {
    char ch = static_cast<char>(c);

    switch (state) {
      case ParserState::Scanning:
        if (ch == '<') {
          currentTagName.clear();
          currentTagAttribs.clear();
          state = ParserState::InTag;
        }
        break;

      case ParserState::InTag:
        if (ch == '>') {
          std::string lowerTag = currentTagName;
          std::transform(lowerTag.begin(), lowerTag.end(), lowerTag.begin(), ::tolower);
          state = (lowerTag == "a") ? (currentText.clear(), ParserState::InAnchorContent) : ParserState::Scanning;
        } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
          std::string lowerTag = currentTagName;
          std::transform(lowerTag.begin(), lowerTag.end(), lowerTag.begin(), ::tolower);
          if (lowerTag == "a") {
            currentTagAttribs.clear();
            state = ParserState::InTagAttribs;
          } else {
            state = ParserState::Scanning;
          }
        } else {
          currentTagName += ch;
        }
        break;

      case ParserState::InTagAttribs:
        if (ch == '>') {
          currentText.clear();
          state = ParserState::InAnchorContent;
        } else {
          currentTagAttribs += ch;
        }
        break;

      case ParserState::InAnchorContent:
        if (ch == '<') {
          int nextC = file.read();
          if (nextC == -1) break;
          char nextCh = static_cast<char>(nextC);
          if (nextCh == '/') {
            std::string closeTag;
            int tc;
            while ((tc = file.read()) != -1) {
              char tch = static_cast<char>(tc);
              if (tch == '>') break;
              closeTag += tch;
            }
            std::transform(closeTag.begin(), closeTag.end(), closeTag.begin(), ::tolower);
            if (closeTag == "a") {
              std::string href = extractHref(currentTagAttribs);
              std::string cleanText = cleanHtmlText(currentText);
              if (!href.empty() && !cleanText.empty()) {
                std::string targetUrl = href;
                size_t uddgPos = targetUrl.find("uddg=");
                if (uddgPos != std::string::npos) {
                  size_t start = uddgPos + 5;
                  size_t end = targetUrl.find('&', start);
                  std::string encoded =
                      (end == std::string::npos) ? targetUrl.substr(start) : targetUrl.substr(start, end - start);
                  targetUrl = urlDecode(encoded);
                }
                if (targetUrl.rfind("http", 0) == 0) {
                  bool dup = false;
                  for (const auto& l : links) {
                    if (l.url == targetUrl) {
                      dup = true;
                      break;
                    }
                  }
                  if (!dup) links.push_back({cleanText, targetUrl});
                }
              }
              state = ParserState::Scanning;
            } else {
              currentText += "</" + closeTag + ">";
            }
          } else {
            // Nested tag opening inside the anchor text, skip until '>'.
            int tc;
            while ((tc = file.read()) != -1) {
              if (static_cast<char>(tc) == '>') break;
            }
          }
        } else {
          currentText += ch;
        }
        break;
    }
  }

  file.close();
  return !links.empty();
}

}  // namespace

void DuckDuckGoActivity::loadRecentSearches() {
  recentSearches.clear();
  HalFile f;
  if (!Storage.openFileForRead("DDG", "/apps/duckduckgo/recent.txt", f)) return;

  std::string line;
  while (f.available() > 0) {
    char c = f.read();
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) recentSearches.push_back(line);
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) recentSearches.push_back(line);
  f.close();
}

void DuckDuckGoActivity::saveRecentSearches() {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/duckduckgo");
  HalFile f;
  if (Storage.openFileForWrite("DDG", "/apps/duckduckgo/recent.txt", f)) {
    for (const auto& query : recentSearches) {
      std::string line = query + "\n";
      f.write(line.c_str(), line.length());
    }
    f.close();
  }
}

void DuckDuckGoActivity::rememberSearch(const std::string& query) {
  recentSearches.erase(std::remove(recentSearches.begin(), recentSearches.end(), query), recentSearches.end());
  recentSearches.insert(recentSearches.begin(), query);
  if (recentSearches.size() > kMaxRecentSearches) recentSearches.resize(kMaxRecentSearches);
  saveRecentSearches();
}

void DuckDuckGoActivity::promptSearch() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_DDG_SEARCH), "", 40);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
    if (!keyboardResult || keyboardResult->text.empty()) {
      requestUpdate();
      return;
    }
    runSearch(keyboardResult->text);
  });
}

void DuckDuckGoActivity::runSearch(const std::string& query) {
  searchQuery = query;
  rememberSearch(query);
  errorMessage.clear();
  state = DDGState::Loading;
  requestUpdate();
  ensureWifiThenFetch();
}

void DuckDuckGoActivity::ensureWifiThenFetch() {
  if (WiFi.status() == WL_CONNECTED) {
    doFetch();
    return;
  }

  WiFi.mode(WIFI_STA);
  startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                          [this](const ActivityResult& result) {
                            if (result.isCancelled) {
                              state = DDGState::SearchResults;
                              if (errorMessage.empty()) errorMessage = tr(STR_DDG_WIFI_REQUIRED);
                              requestUpdate();
                            } else {
                              doFetch();
                            }
                          });
}

void DuckDuckGoActivity::doFetch() {
  requestUpdateAndWait();  // paint the "Loading" state before the blocking calls below

  wifiWasUsed = true;
  bool success = false;
  int retries = 3;
  while (retries-- > 0) {
    success = fetchSearchData();
    if (success) break;
    if (retries > 0) delay(1500);
  }

  if (!success) {
    searchResults.clear();
    if (errorMessage.empty()) errorMessage = tr(STR_DDG_SEARCH_FAILED);
  } else {
    errorMessage.clear();
  }
  selectedIndex = 0;
  state = DDGState::SearchResults;
  requestUpdate();
}

bool DuckDuckGoActivity::fetchSearchData() {
  std::string url = "https://html.duckduckgo.com/html/?q=" + urlEncode(searchQuery);
  const char* tempPath = "/apps/duckduckgo/search.tmp";

  auto result = HttpDownloader::downloadToFile(url, tempPath);
  if (result != HttpDownloader::OK) {
    errorMessage = tr(STR_DDG_SEARCH_FAILED);
    Storage.remove(tempPath);
    return false;
  }

  bool parsed = parseDuckDuckGoResults(tempPath, searchResults);
  Storage.remove(tempPath);
  return parsed;
}

void DuckDuckGoActivity::showQrForResult(int index) {
  if (index < 0 || index >= static_cast<int>(searchResults.size())) return;
  const std::string url = searchResults[index].url;
  if (url.empty()) return;
  startActivityForResult(makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, url),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void DuckDuckGoActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/duckduckgo");

  errorMessage.clear();
  loadRecentSearches();
  state = DDGState::RecentSearches;
  selectedIndex = 0;

  requestUpdate();
}

void DuckDuckGoActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
}

void DuckDuckGoActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == DDGState::Loading) return;  // owned by the blocking doFetch() call that triggered it

  if (mappedInput.wasReleased(Button::Back)) {
    if (state == DDGState::SearchResults) {
      state = DDGState::RecentSearches;
      loadRecentSearches();
      selectedIndex = 0;
      requestUpdate();
    } else {
      onGoHome(HomeMenuItem::APPS_MENU);
    }
    return;
  }

  if (state == DDGState::RecentSearches) {
    int totalItems = static_cast<int>(recentSearches.size()) + 1;  // row 0 = "+ Search"
    if (mappedInput.wasReleased(Button::Up)) {
      selectedIndex = (selectedIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      selectedIndex = (selectedIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (selectedIndex == 0) {
        promptSearch();
      } else {
        runSearch(recentSearches[selectedIndex - 1]);
      }
    }
    return;
  }

  if (state == DDGState::SearchResults) {
    if (!errorMessage.empty()) {
      if (mappedInput.wasReleased(Button::Right) || mappedInput.wasReleased(Button::Down)) {
        runSearch(searchQuery);
      }
      return;
    }
    if (searchResults.empty()) {
      if (mappedInput.wasReleased(Button::Confirm)) promptSearch();
      return;
    }
    int totalItems = static_cast<int>(searchResults.size());
    if (mappedInput.wasReleased(Button::Up)) {
      selectedIndex = (selectedIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      selectedIndex = (selectedIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      showQrForResult(selectedIndex);
    }
    return;
  }
}

void DuckDuckGoActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DDG_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  if (state == DDGState::Loading) {
    int textY = contentTop + contentHeight / 2 - 20;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_DDG_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == DDGState::RecentSearches) {
    GUI.drawButtonMenu(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(recentSearches.size()) + 1,
        selectedIndex,
        [this](int index) {
          if (index == 0) return std::string(tr(STR_DDG_SEARCH_PROMPT));
          return recentSearches[index - 1];
        },
        [](int index) { return index == 0 ? UIIcon::DuckDuckGo : UIIcon::File; });

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == DDGState::SearchResults) {
    if (!errorMessage.empty()) {
      int textY = contentTop + contentHeight / 2 - 40;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, errorMessage.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, textY + 30, tr(STR_DDG_RETRY_HINT));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, tr(STR_HN_REFRESH));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else if (searchResults.empty()) {
      int textY = contentTop + contentHeight / 2 - 20;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_DDG_NO_RESULTS));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawButtonMenu(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(searchResults.size()),
          selectedIndex, [this](int index) { return searchResults[index].title; },
          [](int) { return UIIcon::DuckDuckGo; });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DDG_SHOW_QR), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }

  renderer.displayBuffer();
}

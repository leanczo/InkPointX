#include "WikipediaActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TxtReaderActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
std::string sanitizeFilename(const std::string& title) {
  std::string filename;
  for (char c : title) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') {
      filename += c;
    }
  }
  return filename;
}

// Strips/normalizes the handful of non-ASCII punctuation marks Wikipedia's
// extract text actually contains (smart quotes, dashes, NBSP, zero-width
// space) into plain ASCII, so the reader's own text layout (which assumes a
// narrower glyph set for this font) doesn't have to carry the rest of
// Unicode just for these. Any other multi-byte sequence passes through
// unchanged.
std::string cleanUnicode(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for (size_t i = 0; i < input.size();) {
    unsigned char c = input[i];
    if (c < 0x80) {
      output += static_cast<char>(c);
      i++;
    } else if ((c & 0xE0) == 0xC0) {  // 2 bytes
      if (i + 1 < input.size()) {
        unsigned char c2 = input[i + 1];
        uint16_t codepoint = ((c & 0x1F) << 6) | (c2 & 0x3F);
        if (codepoint == 0x00A0) {  // Non-breaking space
          output += ' ';
        } else {
          output += input.substr(i, 2);
        }
      }
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {  // 3 bytes
      if (i + 2 < input.size()) {
        unsigned char c2 = input[i + 1];
        unsigned char c3 = input[i + 2];
        uint16_t codepoint = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        if (codepoint == 0x2018 || codepoint == 0x2019) {  // Single quotes
          output += '\'';
        } else if (codepoint == 0x201C || codepoint == 0x201D) {  // Double quotes
          output += '"';
        } else if (codepoint == 0x2013 || codepoint == 0x2014) {  // En/em dash
          output += '-';
        } else if (codepoint == 0x200B) {  // Zero-width space
          // skip it
        } else {
          output += input.substr(i, 3);
        }
      }
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {  // 4 bytes
      if (i + 3 < input.size()) {
        output += input.substr(i, 4);
      }
      i += 4;
    } else {
      i++;
    }
  }
  return output;
}

std::string getArticleFilePath(const std::string& title) {
  return "/apps/wikipedia/" + sanitizeFilename(title) + ".md";
}

std::string urlEncode(const std::string& value) {
  std::string escaped;
  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped += c;
    } else if (c == ' ') {
      escaped += "%20";
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
      escaped += hex;
    }
  }
  return escaped;
}

// Hand-rolled streaming JSON scanner for the query/extracts response shape
// ({"query":{"pages":{"NNN":{"title":"...","extract":"..."}}}}). Feeding this
// through ArduinoJson would need the whole (multi-KB, sometimes much larger
// for a long article) extract text resident in RAM twice — once as the parsed
// tree, once again as the std::string pulled out of it. This instead streams
// straight from the downloaded file into the output .md file a line at a
// time, so peak RAM stays a handful of small buffers regardless of article
// length.
bool parseAndSaveWikipediaArticle(const std::string& tempJsonPath, std::string& outTitle) {
  HalFile jsonFile;
  if (!Storage.openFileForRead("WIKI", tempJsonPath.c_str(), jsonFile)) {
    return false;
  }

  std::string tempMdPath = "/apps/wikipedia/md.tmp";
  HalFile mdFile;
  if (!Storage.openFileForWrite("WIKI", tempMdPath.c_str(), mdFile)) {
    jsonFile.close();
    return false;
  }

  enum class ParserState { Scanning, InString, AfterString, ExpectingColon, ExpectingValue, InValueString };

  ParserState state = ParserState::Scanning;
  std::string currentKey;
  std::string currentValue;
  std::string currentLine;
  bool inEscape = false;
  outTitle.clear();

  auto writeLineToMd = [&](const std::string& rawLine) {
    std::string line = cleanUnicode(rawLine);
    if (line.empty()) {
      mdFile.write("\n", 1);
      return;
    }

    size_t startEquals = 0;
    while (startEquals < line.size() && line[startEquals] == '=') startEquals++;
    size_t endEquals = 0;
    while (endEquals < line.size() && line[line.size() - 1 - endEquals] == '=') endEquals++;

    if (startEquals >= 2 && startEquals == endEquals && startEquals < line.size()) {
      std::string headingText = line.substr(startEquals, line.size() - 2 * startEquals);
      size_t first = headingText.find_first_not_of(" ");
      size_t last = headingText.find_last_not_of(" ");
      if (first != std::string::npos && last != std::string::npos) {
        headingText = headingText.substr(first, (last - first + 1));
      }
      std::string mdHeading(startEquals, '#');
      std::string formatted = mdHeading + " " + headingText + "\n\n";
      mdFile.write(formatted.data(), formatted.size());
    } else {
      std::string formatted = line + "\n";
      mdFile.write(formatted.data(), formatted.size());
    }
  };

  int c;
  while ((c = jsonFile.read()) != -1) {
    char ch = static_cast<char>(c);

    switch (state) {
      case ParserState::Scanning:
        if (ch == '"') {
          currentKey.clear();
          state = ParserState::InString;
        }
        break;

      case ParserState::InString:
        if (inEscape) {
          currentKey += ch;
          inEscape = false;
        } else if (ch == '\\') {
          inEscape = true;
        } else if (ch == '"') {
          state = ParserState::AfterString;
        } else {
          currentKey += ch;
        }
        break;

      case ParserState::AfterString:
        if (std::isspace(static_cast<unsigned char>(ch))) {
          // ignore
        } else if (ch == ':') {
          state = ParserState::ExpectingColon;
        } else {
          state = ParserState::Scanning;
        }
        break;

      case ParserState::ExpectingColon:
        if (std::isspace(static_cast<unsigned char>(ch))) {
          // ignore
        } else if (ch == '"') {
          currentValue.clear();
          currentLine.clear();
          inEscape = false;
          if (currentKey == "title" || currentKey == "extract") {
            state = ParserState::InValueString;
          } else {
            state = ParserState::ExpectingValue;
          }
        } else {
          state = ParserState::Scanning;
        }
        break;

      case ParserState::ExpectingValue:
        if (inEscape) {
          inEscape = false;
        } else if (ch == '\\') {
          inEscape = true;
        } else if (ch == '"') {
          state = ParserState::Scanning;
        }
        break;

      case ParserState::InValueString:
        if (inEscape) {
          char escChar = 0;
          if (ch == 'n') {
            escChar = '\n';
          } else if (ch == 'r') {
            escChar = '\r';
          } else if (ch == 't') {
            escChar = '\t';
          } else if (ch == '"') {
            escChar = '"';
          } else if (ch == '\\') {
            escChar = '\\';
          } else if (ch == '/') {
            escChar = '/';
          } else if (ch == 'u') {
            uint16_t codepoint = 0;
            bool ok = true;
            for (int k = 0; k < 4; k++) {
              int hexDigit = jsonFile.read();
              if (hexDigit == -1) {
                ok = false;
                break;
              }
              char h = static_cast<char>(hexDigit);
              codepoint <<= 4;
              if (h >= '0' && h <= '9') {
                codepoint |= (h - '0');
              } else if (h >= 'a' && h <= 'f') {
                codepoint |= (h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                codepoint |= (h - 'A' + 10);
              } else {
                ok = false;
                break;
              }
            }
            if (ok) {
              std::string utf8Str;
              if (codepoint < 0x80) {
                utf8Str += static_cast<char>(codepoint);
              } else if (codepoint < 0x800) {
                utf8Str += static_cast<char>(0xC0 | (codepoint >> 6));
                utf8Str += static_cast<char>(0x80 | (codepoint & 0x3F));
              } else {
                utf8Str += static_cast<char>(0xE0 | (codepoint >> 12));
                utf8Str += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                utf8Str += static_cast<char>(0x80 | (codepoint & 0x3F));
              }

              if (currentKey == "title") {
                currentValue += utf8Str;
              } else if (currentKey == "extract") {
                currentLine += utf8Str;
              }
            }
          } else {
            escChar = ch;
          }

          if (escChar != 0) {
            if (currentKey == "title") {
              currentValue += escChar;
            } else if (currentKey == "extract") {
              if (escChar == '\n') {
                writeLineToMd(currentLine);
                currentLine.clear();
              } else {
                currentLine += escChar;
              }
            }
          }
          inEscape = false;
        } else if (ch == '\\') {
          inEscape = true;
        } else if (ch == '"') {
          if (currentKey == "title") {
            outTitle = currentValue;
          } else if (currentKey == "extract") {
            if (!currentLine.empty()) {
              writeLineToMd(currentLine);
              currentLine.clear();
            }
          }
          state = ParserState::Scanning;
        } else {
          if (currentKey == "title") {
            currentValue += ch;
          } else if (currentKey == "extract") {
            if (ch == '\n') {
              writeLineToMd(currentLine);
              currentLine.clear();
            } else {
              currentLine += ch;
            }
          }
        }
        break;
    }
  }

  jsonFile.close();
  mdFile.close();

  if (outTitle.empty()) {
    Storage.remove(tempMdPath.c_str());
    return false;
  }

  std::string finalPath = "/apps/wikipedia/" + sanitizeFilename(outTitle) + ".md";
  HalFile finalFile;
  if (!Storage.openFileForWrite("WIKI", finalPath.c_str(), finalFile)) {
    Storage.remove(tempMdPath.c_str());
    return false;
  }

  std::string titleHeader = "# " + outTitle + "\n\n";
  finalFile.write(titleHeader.data(), titleHeader.size());

  HalFile tempMdFile;
  if (Storage.openFileForRead("WIKI", tempMdPath.c_str(), tempMdFile)) {
    char copyBuf[512];
    int bytesRead;
    while ((bytesRead = tempMdFile.read(reinterpret_cast<uint8_t*>(copyBuf), sizeof(copyBuf))) > 0) {
      finalFile.write(copyBuf, bytesRead);
    }
    tempMdFile.close();
  }
  finalFile.close();
  Storage.remove(tempMdPath.c_str());

  return true;
}
}  // namespace

void WikipediaActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/wikipedia");

  errorMessage.clear();
  loadOfflineArticlesList();
  state = WikiState::OfflineList;
  selectedIndex = 0;

  requestUpdate();
}

void WikipediaActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void WikipediaActivity::loadOfflineArticlesList() {
  offlineArticles.clear();
  std::vector<String> files = Storage.listFiles("/apps/wikipedia");
  for (const auto& file : files) {
    std::string filename = file.c_str();
    if (filename.length() > 3 && filename.substr(filename.length() - 3) == ".md") {
      offlineArticles.push_back(filename.substr(0, filename.length() - 3));
    }
  }
  std::sort(offlineArticles.begin(), offlineArticles.end());
}

void WikipediaActivity::promptSearch() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WIKIPEDIA_SEARCH), "", 40);
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
    searchQuery = keyboardResult->text;
    isSearchTask = true;
    stateBeforeFetch = state;
    state = WikiState::Loading;
    requestUpdate();
    ensureWifiThenFetch();
  });
}

void WikipediaActivity::openArticle(const std::string& title) {
  std::string filepath = getArticleFilePath(title);
  if (Storage.exists(filepath.c_str())) {
    auto txt = makeUniqueNoThrow<Txt>(filepath, "/.crosspoint");
    if (txt && txt->load()) {
      activityManager.pushActivity(makeUniqueNoThrow<TxtReaderActivity>(renderer, mappedInput, std::move(txt), 0));
    }
    return;
  }

  articleToFetch = title;
  isSearchTask = false;
  errorMessage.clear();
  stateBeforeFetch = WikiState::SearchResults;
  state = WikiState::Loading;
  requestUpdate();
  ensureWifiThenFetch();
}

void WikipediaActivity::ensureWifiThenFetch() {
  if (WiFi.status() == WL_CONNECTED) {
    doFetch();
    return;
  }

  WiFi.mode(WIFI_STA);
  startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                          [this](const ActivityResult& result) {
                            if (result.isCancelled) {
                              state = stateBeforeFetch;
                              requestUpdate();
                            } else {
                              wifiWasUsed = true;
                              doFetch();
                            }
                          });
}

void WikipediaActivity::doFetch() {
  requestUpdateAndWait();  // paint the "Loading..." state before the blocking calls below

  bool success = false;
  int retries = 3;
  while (retries-- > 0) {
    success = isSearchTask ? fetchSearchData() : fetchArticleData();
    if (success) break;
    if (retries > 0) delay(1500);
  }

  if (isSearchTask) {
    if (!success) {
      searchResults.clear();
      if (errorMessage.empty()) errorMessage = tr(STR_WIKIPEDIA_SEARCH_FAILED);
    } else {
      errorMessage.clear();
    }
    selectedIndex = 0;
    state = WikiState::SearchResults;
    requestUpdate();
    return;
  }

  if (!success) {
    if (errorMessage.empty()) errorMessage = tr(STR_WIKIPEDIA_DOWNLOAD_FAILED);
    state = WikiState::SearchResults;
    requestUpdate();
    return;
  }

  std::string filepath = getArticleFilePath(currentArticleTitle);
  auto txt = makeUniqueNoThrow<Txt>(filepath, "/.crosspoint");
  state = WikiState::SearchResults;
  if (txt && txt->load()) {
    requestUpdate();
    activityManager.pushActivity(makeUniqueNoThrow<TxtReaderActivity>(renderer, mappedInput, std::move(txt), 0));
    return;
  }
  requestUpdate();
}

bool WikipediaActivity::fetchSearchData() {
  std::string url =
      "https://en.wikipedia.org/w/api.php?action=opensearch&search=" + urlEncode(searchQuery) + "&limit=10&namespace=0&format=json";
  const char* tempPath = "/apps/wikipedia/search.tmp";

  auto result = HttpDownloader::downloadToFile(url, tempPath);
  if (result != HttpDownloader::OK) {
    errorMessage = tr(STR_WIKIPEDIA_SEARCH_FAILED);
    Storage.remove(tempPath);
    return false;
  }

  HalFile file;
  bool ok = false;
  if (Storage.openFileForRead("WIKI", tempPath, file)) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (!err && doc.is<JsonArray>()) {
      JsonArray arr = doc.as<JsonArray>();
      if (arr.size() >= 2) {
        JsonArray titles = arr[1].as<JsonArray>();
        searchResults.clear();
        for (JsonVariant val : titles) {
          searchResults.push_back(val.as<std::string>());
        }
        ok = true;
      }
    }
  }
  Storage.remove(tempPath);
  return ok;
}

bool WikipediaActivity::fetchArticleData() {
  std::string url = "https://en.wikipedia.org/w/api.php?action=query&prop=extracts&explaintext=&titles=" +
                     urlEncode(articleToFetch) + "&format=json&redirects=1";
  const char* tempPath = "/apps/wikipedia/article.tmp";

  auto result = HttpDownloader::downloadToFile(url, tempPath);
  if (result != HttpDownloader::OK) {
    errorMessage = tr(STR_WIKIPEDIA_DOWNLOAD_FAILED);
    Storage.remove(tempPath);
    return false;
  }

  std::string title;
  bool ok = parseAndSaveWikipediaArticle(tempPath, title);
  Storage.remove(tempPath);
  if (ok && !title.empty()) {
    currentArticleTitle = title;
    return true;
  }
  return false;
}

void WikipediaActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == WikiState::SearchResults) {
      state = WikiState::OfflineList;
      loadOfflineArticlesList();
      selectedIndex = 0;
      requestUpdate();
    } else {
      onGoHome(HomeMenuItem::APPS_MENU);
    }
    return;
  }

  if (state == WikiState::OfflineList) {
    int totalItems = static_cast<int>(offlineArticles.size()) + 1;
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedIndex = (selectedIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedIndex = (selectedIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedIndex == 0) {
        promptSearch();
      } else {
        openArticle(offlineArticles[selectedIndex - 1]);
      }
    }
    return;
  }

  if (state == WikiState::SearchResults) {
    if (!errorMessage.empty()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
          mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        isSearchTask = true;
        errorMessage.clear();
        stateBeforeFetch = WikiState::SearchResults;
        state = WikiState::Loading;
        requestUpdate();
        ensureWifiThenFetch();
      }
      return;
    }
    if (searchResults.empty()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        promptSearch();
      }
      return;
    }
    int totalItems = static_cast<int>(searchResults.size());
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedIndex = (selectedIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedIndex = (selectedIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openArticle(searchResults[selectedIndex]);
    }
    return;
  }
}

void WikipediaActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WIKIPEDIA_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  if (state == WikiState::Loading) {
    int textY = contentTop + contentHeight / 2 - 20;
    renderer.drawCenteredText(UI_12_FONT_ID, textY,
                               isSearchTask ? tr(STR_WIKIPEDIA_LOADING_SEARCH) : tr(STR_WIKIPEDIA_DOWNLOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == WikiState::OfflineList) {
    GUI.drawButtonMenu(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(offlineArticles.size()) + 1,
        selectedIndex,
        [this](int index) {
          if (index == 0) return std::string(tr(STR_WIKIPEDIA_SEARCH_PROMPT));
          return offlineArticles[index - 1];
        },
        [](int index) { return index == 0 ? UIIcon::File : UIIcon::Book; });

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == WikiState::SearchResults) {
    if (!errorMessage.empty()) {
      int textY = contentTop + contentHeight / 2 - 40;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, errorMessage.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, textY + 30, tr(STR_WIKIPEDIA_RETRY_HINT));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, tr(STR_CALENDAR_REFRESH));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else if (searchResults.empty()) {
      int textY = contentTop + contentHeight / 2 - 20;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_WIKIPEDIA_NO_RESULTS));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawButtonMenu(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(searchResults.size()),
          selectedIndex, [this](int index) { return searchResults[index]; }, [](int) { return UIIcon::Book; });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }

  renderer.displayBuffer();
}

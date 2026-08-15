#include "RssActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <XmlParserUtils.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

// Selectable font sizes for the article detail view, cycled with Left/Right.
// Default (index 0) is UI_10_FONT_ID -- the same font the feed listing uses
// -- so opening an article doesn't visibly change the text size; Right steps
// up through progressively larger NotoSans sizes for anyone who wants it.
constexpr int kRssArticleFontIds[] = {UI_10_FONT_ID, NOTOSANS_12_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_16_FONT_ID,
                                      NOTOSANS_18_FONT_ID};
constexpr size_t kRssArticleFontCount = sizeof(kRssArticleFontIds) / sizeof(kRssArticleFontIds[0]);
constexpr uint8_t kRssDefaultArticleFontIndex = 0;

// Decodes a numeric (&#NNN;) or hex (&#xNNN;) character reference, or one of
// the common named entities listed below, into its UTF-8 byte sequence.
// `name` is the entity body without the leading '&' or trailing ';' (e.g.
// "amp", "#39", "#x2019"). Returns true and fills `outUtf8` on success.
bool decodeHtmlEntity(const std::string& name, std::string& outUtf8) {
  int code = 0;
  bool decoded = false;
  if (!name.empty() && name[0] == '#') {
    decoded = true;
    if (name.length() > 2 && (name[1] == 'x' || name[1] == 'X')) {
      for (size_t k = 2; k < name.length(); k++) {
        char ch = name[k];
        if (ch >= '0' && ch <= '9')
          code = code * 16 + (ch - '0');
        else if (ch >= 'a' && ch <= 'f')
          code = code * 16 + (ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
          code = code * 16 + (ch - 'A' + 10);
      }
    } else {
      for (size_t k = 1; k < name.length(); k++) {
        char ch = name[k];
        if (ch >= '0' && ch <= '9') code = code * 10 + (ch - '0');
      }
    }
  } else if (name == "amp") {
    code = 38;
    decoded = true;
  } else if (name == "lt") {
    code = 60;
    decoded = true;
  } else if (name == "gt") {
    code = 62;
    decoded = true;
  } else if (name == "quot") {
    code = 34;
    decoded = true;
  } else if (name == "apos") {
    code = 39;
    decoded = true;
  } else if (name == "nbsp") {
    code = 32;
    decoded = true;
  } else if (name == "ldquo") {
    code = 8220;
    decoded = true;
  } else if (name == "rdquo") {
    code = 8221;
    decoded = true;
  } else if (name == "lsquo") {
    code = 8216;
    decoded = true;
  } else if (name == "rsquo") {
    code = 8217;
    decoded = true;
  } else if (name == "ndash") {
    code = 8211;
    decoded = true;
  } else if (name == "mdash") {
    code = 8212;
    decoded = true;
  } else if (name == "hellip") {
    code = 8230;
    decoded = true;
  } else if (name == "euro") {
    code = 8364;
    decoded = true;
  } else if (name == "copy") {
    code = 169;
    decoded = true;
  } else if (name == "reg") {
    code = 174;
    decoded = true;
  } else if (name == "trade") {
    code = 8482;
    decoded = true;
  }

  if (!decoded || code <= 0) return false;

  if (code <= 0x7F) {
    outUtf8 += static_cast<char>(code);
  } else if (code <= 0x7FF) {
    outUtf8 += static_cast<char>(0xC0 | ((code >> 6) & 0x1F));
    outUtf8 += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code <= 0xFFFF) {
    outUtf8 += static_cast<char>(0xE0 | ((code >> 12) & 0x0F));
    outUtf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    outUtf8 += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    outUtf8 += static_cast<char>(0xF0 | ((code >> 18) & 0x07));
    outUtf8 += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    outUtf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    outUtf8 += static_cast<char>(0x80 | (code & 0x3F));
  }
  return true;
}

// Block-level tags whose open AND close both mark a paragraph/line boundary
// in the source HTML -- stripping them without a trace (the old behavior)
// ran unrelated paragraphs together. <p>...</p> yields two boundary hits
// (open, then close) so consecutive paragraphs get a blank line between
// them; a lone self-closing <br/> yields one hit, a soft line break.
bool isBlockBoundaryTag(const std::string& lowerTagName) {
  return lowerTagName == "p" || lowerTagName == "div" || lowerTagName == "li" || lowerTagName == "tr" ||
         lowerTagName == "br" || lowerTagName == "blockquote" || lowerTagName == "h1" || lowerTagName == "h2" ||
         lowerTagName == "h3" || lowerTagName == "h4" || lowerTagName == "h5" || lowerTagName == "h6";
}

bool isHeadingTag(const std::string& lowerTagName) {
  return lowerTagName == "h1" || lowerTagName == "h2" || lowerTagName == "h3" || lowerTagName == "h4" ||
         lowerTagName == "h5" || lowerTagName == "h6";
}

// Marks the very first character of a paragraph as a heading -- stripped
// back out (and turned into a bold line instead of just plain text) by
// wrapParagraphs() below. A raw SOH byte never occurs in real feed text, and
// survives the round trip through the markdown cache file unaffected: only
// '\n' is escaped there (see RssParser::writeItem/unescapeNewlines).
constexpr char kMarkdownHeadingSentinel = '\x01';

std::string cleanField(const std::string& input) {
  std::string clean;
  clean.reserve(input.length());

  bool lastWasSpace = true;
  bool inTag = false;
  bool tagIsClosing = false;
  std::string currentTagName;
  // Some feeds (notably markdown-formatted ones, with no HTML tags at all)
  // mark paragraph breaks with a blank line in the raw text instead of a
  // block tag. Counts consecutive '\n' within the whitespace run currently
  // being collapsed so a genuine blank line can be told apart from an
  // incidental single line-wrap in the source XML.
  int newlineRun = 0;

  size_t i = 0;
  while (i < input.length()) {
    char c = input[i];

    // Handle HTML entities: named (&amp; &nbsp; &rsquo; &mdash; ...), decimal
    // (&#8217;) and hex (&#x2019;) character references, decoded straight to
    // UTF-8. Entities this doesn't recognize are left as literal text below
    // rather than silently dropped.
    if (c == '&') {
      std::string entity;
      size_t j = i;
      while (j < input.length() && j - i < 10) {
        char ec = input[j];
        entity += ec;
        if (ec == ';') break;
        j++;
      }

      std::string replacement;
      size_t entityLen = 0;
      if (entity.length() >= 3 && entity.back() == ';') {
        const std::string name = entity.substr(1, entity.length() - 2);
        if (decodeHtmlEntity(name, replacement)) {
          entityLen = entity.length();
        }
      }

      if (!replacement.empty()) {
        for (char rc : replacement) {
          if (rc == ' ') {
            if (!lastWasSpace) {
              clean += ' ';
              lastWasSpace = true;
            }
          } else {
            clean += rc;
            lastWasSpace = false;
          }
        }
        i += entityLen;
        continue;
      }
      i++;
    } else {
      i++;
    }

    // Handle tag stripping
    if (c == '<') {
      inTag = true;
      currentTagName.clear();
      tagIsClosing = false;
      continue;
    } else if (c == '>') {
      inTag = false;
      std::string lowerTagName = currentTagName;
      for (char& tc : lowerTagName) tc = static_cast<char>(std::tolower(static_cast<unsigned char>(tc)));
      if (isBlockBoundaryTag(lowerTagName)) {
        if (!clean.empty() && clean.back() != '\n') {
          clean += '\n';
          lastWasSpace = true;
        }
        // Only on the opening tag, so the sentinel lands on the heading
        // paragraph's first character rather than trailing its last one.
        if (isHeadingTag(lowerTagName) && !tagIsClosing) clean += kMarkdownHeadingSentinel;
      }
      continue;
    }

    if (inTag) {
      // Collect the tag name (e.g. "p" from both "<p>" and "</p>" -- the
      // leading '/' of a closing tag isn't alnum so it's simply skipped,
      // just noted below) stopping at the first attribute space so
      // "<p class=...>" still yields "p".
      if (currentTagName.empty() && c == '/') {
        tagIsClosing = true;
      } else if (currentTagName.empty() ? std::isalpha(static_cast<unsigned char>(c))
                                        : std::isalnum(static_cast<unsigned char>(c))) {
        currentTagName += c;
      }
      continue;
    }

    // Replace whitespace characters and collapse multiple spaces
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (c == '\n') newlineRun++;
      if (!lastWasSpace) {
        clean += ' ';
        lastWasSpace = true;
      }
    } else {
      // A blank line (2+ newlines) in the run just collapsed to a single
      // space is a real paragraph break, not a soft line-wrap -- upgrade it.
      if (newlineRun >= 2 && !clean.empty() && clean.back() == ' ') {
        clean.back() = '\n';
      }
      newlineRun = 0;
      clean += c;
      lastWasSpace = false;
    }
  }

  while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\n')) {
    clean.pop_back();
  }
  return clean;
}

// Some feeds emit the article body as literal Markdown instead of HTML, so
// "### Heading" needs to be recognized before cleanField() collapses line
// breaks -- an ATX header only means anything as the first thing on its own
// RAW line. Many generators skip the blank line the markdown spec technically
// wants around a heading, so this doesn't require one: it forces one itself
// (cleanField()'s blank-line paragraph-break detection then reliably isolates
// the heading either way) and marks the line with the same sentinel
// cleanField() uses for HTML <h1>-<h6>, so wrapParagraphs() renders both the
// same way -- as their own bold line, a real heading instead of just having
// its "#"s deleted. Must run before cleanField() on the untouched field text.
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
      result += kMarkdownHeadingSentinel;
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

// Cleans up the remaining markdown punctuation cleanField() leaves untouched
// (it only strips HTML tags). Applied only to description/content -- title
// and link are never markdown. Headers are already handled by
// promoteMarkdownHeaders() above, before cleanField() ever runs.
void stripMarkdownArtifacts(std::string& text) {
  std::string result = text;

  // Markdown links: [text](url) -> text. The exact "](" sequence essentially
  // never appears in ordinary prose, so no paired-marker lookahead is needed.
  size_t bracketOpen = 0;
  while ((bracketOpen = result.find('[', bracketOpen)) != std::string::npos) {
    size_t bracketClose = result.find(']', bracketOpen + 1);
    if (bracketClose == std::string::npos) break;
    if (bracketClose + 1 >= result.length() || result[bracketClose + 1] != '(') {
      bracketOpen = bracketClose + 1;
      continue;
    }
    size_t parenClose = result.find(')', bracketClose + 2);
    if (parenClose == std::string::npos) break;
    std::string linkText = result.substr(bracketOpen + 1, bracketClose - bracketOpen - 1);
    result.replace(bracketOpen, parenClose - bracketOpen + 1, linkText);
    bracketOpen += linkText.length();
  }

  // Bold markers only (** and __) -- not single */_ italics, which collide
  // too often with ordinary prose (stray asterisks, math, informal emphasis)
  // to safely guess a real pair. Only strips markers with a matching close,
  // so an unpaired marker is left alone rather than eating the rest of the text.
  auto stripPaired = [](std::string& s, const char* marker) {
    const size_t markerLen = std::strlen(marker);
    size_t pos = 0;
    while ((pos = s.find(marker, pos)) != std::string::npos) {
      size_t close = s.find(marker, pos + markerLen);
      if (close == std::string::npos) break;
      s.erase(close, markerLen);
      s.erase(pos, markerLen);
    }
  };
  stripPaired(result, "**");
  stripPaired(result, "__");

  text = std::move(result);
}

// A line of already-wrapped body text plus which font family it should draw
// with -- currently just heading (bold) vs. regular, since GfxRenderer only
// supports one style per drawText() call and has no inline mixed-run text.
struct DetailLine {
  std::string text;
  bool bold;
};

// wrappedText() wraps by width only and has no concept of '\n' as a line
// break -- it would render one embedded in the middle of a "word". This
// splits on the paragraph breaks cleanField()/promoteMarkdownHeaders() leave
// behind and wraps each paragraph independently, passing each the line
// budget remaining out of maxLines so the overall cap (and wrappedText's own
// "..." truncation on the line that hits it) still applies across
// paragraphs, not just within one. A paragraph starting with the heading
// sentinel wraps in bold instead of `style`, with the sentinel itself
// stripped before it ever reaches wrappedText/drawText.
std::vector<DetailLine> wrapParagraphs(const GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth,
                                       int maxLines, EpdFontFamily::Style style) {
  std::vector<DetailLine> allLines;
  size_t start = 0;
  while (start <= text.length() && static_cast<int>(allLines.size()) < maxLines) {
    size_t nl = text.find('\n', start);
    std::string paragraph = (nl == std::string::npos) ? text.substr(start) : text.substr(start, nl - start);

    const bool isHeading = !paragraph.empty() && paragraph.front() == kMarkdownHeadingSentinel;
    if (isHeading) paragraph.erase(0, 1);

    if (paragraph.empty()) {
      allLines.push_back({"", false});
    } else {
      int budget = maxLines - static_cast<int>(allLines.size());
      auto paragraphLines =
          renderer.wrappedText(fontId, paragraph.c_str(), maxWidth, budget, isHeading ? EpdFontFamily::BOLD : style);
      for (auto& line : paragraphLines) allLines.push_back({std::move(line), isHeading});
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return allLines;
}

uint32_t parseRssDateToUnix(const std::string& dateStr) {
  if (dateStr.empty()) return 0;

  int year = 1970, month = 1, day = 1;
  int hour = 0, minute = 0, second = 0;

  if (dateStr.length() >= 10 && dateStr[4] == '-' && dateStr[7] == '-') {
    year = std::atoi(dateStr.substr(0, 4).c_str());
    month = std::atoi(dateStr.substr(5, 2).c_str());
    day = std::atoi(dateStr.substr(8, 2).c_str());
    if (dateStr.length() >= 19 && (dateStr[10] == 'T' || dateStr[10] == ' ')) {
      hour = std::atoi(dateStr.substr(11, 2).c_str());
      minute = std::atoi(dateStr.substr(14, 2).c_str());
      second = std::atoi(dateStr.substr(17, 2).c_str());
    }
  } else {
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    size_t mPos = std::string::npos;
    int mIdx = 0;
    for (int i = 0; i < 12; i++) {
      mPos = dateStr.find(months[i]);
      if (mPos != std::string::npos) {
        mIdx = i + 1;
        break;
      }
    }

    if (mPos != std::string::npos) {
      month = mIdx;

      std::string dayStr;
      int p = static_cast<int>(mPos) - 1;
      while (p >= 0 && std::isspace(static_cast<unsigned char>(dateStr[p]))) p--;
      while (p >= 0 && std::isdigit(static_cast<unsigned char>(dateStr[p]))) {
        dayStr = dateStr[p] + dayStr;
        p--;
      }
      if (!dayStr.empty()) day = std::atoi(dayStr.c_str());

      std::string yearStr;
      size_t yp = mPos + 3;
      while (yp < dateStr.length() && !std::isdigit(static_cast<unsigned char>(dateStr[yp]))) yp++;
      while (yp < dateStr.length() && std::isdigit(static_cast<unsigned char>(dateStr[yp])) && yearStr.length() < 4) {
        yearStr += dateStr[yp];
        yp++;
      }
      if (yearStr.length() == 4) year = std::atoi(yearStr.c_str());

      size_t colonPos = dateStr.find(':');
      if (colonPos != std::string::npos && colonPos >= 2) {
        hour = std::atoi(dateStr.substr(colonPos - 2, 2).c_str());
        minute = std::atoi(dateStr.substr(colonPos + 1, 2).c_str());
        size_t nextColon = dateStr.find(':', colonPos + 1);
        if (nextColon != std::string::npos) {
          second = std::atoi(dateStr.substr(nextColon + 1, 2).c_str());
        }
      }
    }
  }

  if (year < 1970) year = 1970;
  if (month < 1) month = 1;
  if (month > 12) month = 12;
  if (day < 1) day = 1;
  if (day > 31) day = 31;

  static const int daysToMonth[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

  int leapYears = (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
  long days = (year - 1970) * 365 + leapYears + daysToMonth[month] + (day - 1);

  bool isLeap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  if (isLeap && month > 2) days++;

  return days * 86400 + hour * 3600 + minute * 60 + second;
}

std::string getSanitizedUrlFilename(const std::string& url) {
  std::string clean;
  for (char c : url) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      clean += c;
    } else {
      clean += '_';
    }
  }
  std::string result;
  bool lastWasUnderscore = false;
  for (char c : clean) {
    if (c == '_') {
      if (!lastWasUnderscore) {
        result += c;
        lastWasUnderscore = true;
      }
    } else {
      result += c;
      lastWasUnderscore = false;
    }
  }
  if (result.length() > 64) {
    result = result.substr(result.length() - 64);
  }
  return result;
}

std::string getFriendlyFeedName(const std::string& url) {
  std::string text = url;
  for (char& c : text) {
    if (!std::isalnum(static_cast<unsigned char>(c))) c = ' ';
  }

  // Tokenize and keep only the first three words, skipping boilerplates
  // case-insensitively
  std::vector<std::string> words;
  std::string currentWord;
  auto flushWord = [&]() {
    if (currentWord.empty()) return;
    std::string loweredWord = currentWord;
    for (char& wc : loweredWord) wc = std::tolower(wc);
    if (loweredWord != "http" && loweredWord != "https" && loweredWord != "www" && loweredWord != "com" &&
        loweredWord != "rss" && loweredWord != "xml") {
      words.push_back(currentWord);
    }
    currentWord.clear();
  };
  for (char c : text) {
    if (c == ' ') {
      flushWord();
    } else {
      currentWord += c;
    }
  }
  flushWord();

  std::string friendlyName;
  for (size_t i = 0; i < words.size() && i < 3; i++) {
    if (i > 0) friendlyName += " ";
    friendlyName += words[i];
  }
  if (friendlyName.empty()) friendlyName = "Feed";
  return friendlyName;
}

// The markdown cache escapes embedded newlines as literal "\n" (see
// RssParser::writeItem below) so each item stays on a single line for the
// simple line-based scanners here; this undoes that escaping on read.
std::string unescapeNewlines(const std::string& raw) {
  std::string out = raw;
  size_t pos = 0;
  while ((pos = out.find("\\n", pos)) != std::string::npos) {
    out.replace(pos, 2, "\n");
    pos += 1;
  }
  return out;
}

bool loadSingleItemDetails(const std::string& filepath, const std::string& targetLink, std::string& outDesc,
                           std::string& outContent) {
  HalFile file;
  if (!Storage.openFileForRead("RSS", filepath, file)) {
    return false;
  }

  std::string line;
  std::string currentLink;
  std::string currentDesc;
  std::string currentContent;
  bool found = false;

  auto handleLine = [&]() {
    if (line.rfind("## ", 0) == 0) {
      if (found) return;
      currentLink.clear();
      currentDesc.clear();
      currentContent.clear();
    } else if (line.rfind("- Link: ", 0) == 0) {
      currentLink = line.substr(8);
      if (currentLink == targetLink) found = true;
    } else if (found) {
      if (line.rfind("- Description: ", 0) == 0) {
        currentDesc = unescapeNewlines(line.substr(15));
      } else if (line.rfind("- Content: ", 0) == 0) {
        currentContent = unescapeNewlines(line.substr(11));
      }
    }
  };

  while (file.available() > 0) {
    char c = file.read();
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (found && line.rfind("## ", 0) == 0) break;  // reached the next item after the target
      handleLine();
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) handleLine();

  file.close();
  if (found) {
    outDesc = currentDesc;
    outContent = currentContent;
    return true;
  }
  return false;
}

class RssParser {
 public:
  RssParser(HalFile& outFile, const std::string& defaultFeedName)
      : outFile(outFile), defaultFeedName(defaultFeedName) {
    parser = XML_ParserCreate(nullptr);
    if (parser) {
      XML_SetUserData(parser, this);
      XML_SetElementHandler(parser, startElement, endElement);
      XML_SetCharacterDataHandler(parser, characterData);
    }
  }

  ~RssParser() { destroyXmlParser(parser); }

  bool parseBuffer(const char* data, int len, bool isFinal) {
    if (!parser) return false;
    if (XML_Parse(parser, data, len, isFinal) == XML_STATUS_ERROR) {
      LOG_DBG("RSS", "Parse error: %s at line %lu", XML_ErrorString(XML_GetErrorCode(parser)),
              XML_GetCurrentLineNumber(parser));
      return false;
    }
    return true;
  }

  int getItemsParsed() const { return itemsParsed; }

 private:
  HalFile& outFile;
  std::string defaultFeedName;
  XML_Parser parser = nullptr;

  bool inItem = false;
  std::string currentTag;
  std::string currentText;
  RssItem currentItem;
  int itemsParsed = 0;

  void writeItem(const RssItem& item) {
    outFile.print("## ");
    outFile.print(item.title.c_str());
    outFile.print("\n- Link: ");
    outFile.print(item.link.c_str());
    outFile.print("\n- Source: ");
    outFile.print(item.feedName.c_str());
    outFile.print("\n- Timestamp: ");
    outFile.print(item.timestamp.c_str());
    outFile.print("\n");

    outFile.print("- Description: ");
    std::string escapedDesc = item.description;
    size_t npos = 0;
    while ((npos = escapedDesc.find('\n', npos)) != std::string::npos) {
      escapedDesc.replace(npos, 1, "\\n");
      npos += 2;
    }
    outFile.print(escapedDesc.c_str());
    outFile.print("\n");

    outFile.print("- Content: ");
    std::string escapedContent = item.content;
    npos = 0;
    while ((npos = escapedContent.find('\n', npos)) != std::string::npos) {
      escapedContent.replace(npos, 1, "\\n");
      npos += 2;
    }
    outFile.print(escapedContent.c_str());
    outFile.print("\n\n");
  }

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
    auto* self = static_cast<RssParser*>(userData);
    std::string lowerTag(name);
    for (char& c : lowerTag) c = std::tolower(c);

    std::string localTag = lowerTag;
    size_t colonPos = localTag.find(':');
    if (colonPos != std::string::npos) localTag = localTag.substr(colonPos + 1);

    if (localTag == "item" || localTag == "entry") {
      self->inItem = true;
      self->currentItem = RssItem();
      self->currentItem.feedName = self->defaultFeedName;
    }

    if (self->inItem) {
      self->currentTag = localTag;
      self->currentText.clear();

      if (localTag == "link") {
        std::string href, rel;
        for (int i = 0; atts[i]; i += 2) {
          std::string attName(atts[i]);
          for (char& c : attName) c = std::tolower(c);
          std::string localAtt = attName;
          size_t attColon = localAtt.find(':');
          if (attColon != std::string::npos) localAtt = localAtt.substr(attColon + 1);

          if (localAtt == "href") {
            href = atts[i + 1];
          } else if (localAtt == "rel") {
            rel = atts[i + 1];
            for (char& c : rel) c = std::tolower(c);
          }
        }

        if (!href.empty()) {
          // If our current link is empty, or we explicitly got an alternate
          // link, prioritize it. Avoid overwriting a valid link with "self" or
          // "enclosure" feed links.
          if (self->currentItem.link.empty() || rel == "alternate" || (rel != "self" && rel != "enclosure")) {
            self->currentItem.link = href;
          }
        }
      }
    }
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<RssParser*>(userData);
    std::string lowerTag(name);
    for (char& c : lowerTag) c = std::tolower(c);

    std::string localTag = lowerTag;
    size_t colonPos = localTag.find(':');
    if (colonPos != std::string::npos) localTag = localTag.substr(colonPos + 1);

    if (localTag == "item" || localTag == "entry") {
      self->inItem = false;
      if (!self->currentItem.title.empty() && self->itemsParsed < 25) {
        self->currentItem.title = cleanField(self->currentItem.title);
        self->currentItem.link = cleanField(self->currentItem.link);
        self->currentItem.description = cleanField(promoteMarkdownHeaders(self->currentItem.description));
        stripMarkdownArtifacts(self->currentItem.description);
        self->currentItem.content = cleanField(promoteMarkdownHeaders(self->currentItem.content));
        stripMarkdownArtifacts(self->currentItem.content);

        self->writeItem(self->currentItem);
        self->itemsParsed++;
      }
      self->currentItem = RssItem();  // reclaim heap immediately
    } else if (self->inItem) {
      if (localTag == "title") {
        if (self->currentItem.title.empty()) self->currentItem.title = self->currentText;
      } else if (localTag == "link") {
        if (self->currentItem.link.empty()) self->currentItem.link = self->currentText;
      } else if (localTag == "description" || localTag == "summary") {
        if (self->currentItem.description.empty()) self->currentItem.description = self->currentText;
      } else if (localTag == "content" || localTag == "encoded" || lowerTag == "content:encoded") {
        if (self->currentItem.content.empty()) self->currentItem.content = self->currentText;
      } else if (localTag == "pubdate" || localTag == "updated" || localTag == "published" || localTag == "date" ||
                 lowerTag == "dc:date") {
        if (self->currentItem.timestamp.empty()) {
          uint32_t ts = parseRssDateToUnix(cleanField(self->currentText));
          self->currentItem.timestamp = std::to_string(ts);
        }
      }
    }

    self->currentTag.clear();
    self->currentText.clear();
  }

  static void XMLCALL characterData(void* userData, const XML_Char* s, const int len) {
    auto* self = static_cast<RssParser*>(userData);
    if (self->inItem && !self->currentTag.empty()) {
      size_t limit = 8192;
      if (self->currentTag == "description" || self->currentTag == "summary") {
        limit = 2048;
      } else if (self->currentTag == "title" || self->currentTag == "link") {
        limit = 512;
      }
      if (self->currentText.length() < limit) {
        size_t toAppend = std::min(static_cast<size_t>(len), limit - self->currentText.length());
        self->currentText.append(s, toAppend);
      }
    }
  }
};

bool parseXmlFile(const std::string& xmlPath, const std::string& mdPath, const std::string& defaultFeedName) {
  HalFile inFile;
  if (!Storage.openFileForRead("RSS", xmlPath.c_str(), inFile)) return false;

  HalFile outFile;
  if (!Storage.openFileForWrite("RSS", mdPath.c_str(), outFile)) {
    inFile.close();
    return false;
  }

  outFile.print("# ");
  outFile.print(defaultFeedName.c_str());
  outFile.print(" Feed\n\n");

  RssParser parser(outFile, defaultFeedName);
  char buffer[2048];

  while (inFile.available() > 0) {
    int bytesRead = inFile.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
    if (bytesRead > 0) {
      if (!parser.parseBuffer(buffer, bytesRead, inFile.available() == 0)) break;
    }
  }

  inFile.close();
  outFile.close();
  // A download that succeeds but yields zero items is otherwise silent about
  // why -- this is the one line that tells the difference between "the XML
  // parse itself failed partway" (see the parse-error LOG_DBG above) and "it
  // parsed cleanly but nothing matched <item>/<entry>" (wrong content type,
  // an unexpected feed schema, ...).
  LOG_DBG("RSS", "Parsed %d item(s) from %s", parser.getItemsParsed(), xmlPath.c_str());
  return parser.getItemsParsed() > 0;
}

std::string timeAgo(uint32_t timestamp) {
  if (timestamp == 0) return "";
  time_t now = time(nullptr);
  if (now < static_cast<time_t>(timestamp)) {
    struct tm tm_info;
    time_t ts = timestamp;
    gmtime_r(&ts, &tm_info);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour,
             tm_info.tm_min);
    return std::string(buf);
  }
  uint32_t diff = now - timestamp;
  if (diff < 60) return tr(STR_RSS_TIME_NOW);
  char buf[16];
  if (diff < 3600) {
    snprintf(buf, sizeof(buf), tr(STR_RSS_TIME_MINUTES), static_cast<int>(diff / 60));
  } else if (diff < 86400) {
    snprintf(buf, sizeof(buf), tr(STR_RSS_TIME_HOURS), static_cast<int>(diff / 3600));
  } else {
    snprintf(buf, sizeof(buf), tr(STR_RSS_TIME_DAYS), static_cast<int>(diff / 86400));
  }
  return std::string(buf);
}

// Parses one subscriptions.txt line into a subscription. The format is
// "<url>\t<customName>"; a bare URL with no tab (the old, pre-rename format)
// still parses fine with an empty customName. A literal tab byte can't
// appear in a valid URL, making it a safe delimiter.
RssSubscription parseSubscriptionLine(const std::string& line) {
  size_t tabPos = line.find('\t');
  if (tabPos == std::string::npos) return {line, ""};
  return {line.substr(0, tabPos), line.substr(tabPos + 1)};
}

}  // namespace

void RssActivity::ensureDirectoriesExist() {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/rss");
}

void RssActivity::loadSubscriptions() {
  subscriptions.clear();
  HalFile f;
  if (!Storage.openFileForRead("RSS", "/apps/rss/subscriptions.txt", f)) {
    subscriptions.push_back({"https://www.reddit.com/.rss", "Reddit"});
    subscriptions.push_back({"https://hnrss.org/frontpage", "Hacker News"});
    saveSubscriptions();
    return;
  }

  std::string currentLine;
  while (f.available() > 0) {
    char c = f.read();
    if (c == '\n') {
      if (!currentLine.empty() && currentLine.back() == '\r') currentLine.pop_back();
      if (!currentLine.empty()) subscriptions.push_back(parseSubscriptionLine(currentLine));
      currentLine.clear();
    } else {
      currentLine += c;
    }
  }
  if (!currentLine.empty()) subscriptions.push_back(parseSubscriptionLine(currentLine));
  f.close();
}

void RssActivity::saveSubscriptions() {
  HalFile f;
  if (Storage.openFileForWrite("RSS", "/apps/rss/subscriptions.txt", f)) {
    for (const auto& sub : subscriptions) {
      std::string line = sub.url + "\t" + sub.customName + "\n";
      f.write(line.c_str(), line.length());
    }
    f.close();
  }
}

std::string RssActivity::displayNameForUrl(const std::string& url) const {
  for (const auto& sub : subscriptions) {
    if (sub.url == url) return !sub.customName.empty() ? sub.customName : getFriendlyFeedName(url);
  }
  return getFriendlyFeedName(url);
}

void RssActivity::loadArticleFontSize() {
  articleFontSizeIndex = kRssDefaultArticleFontIndex;
  HalFile f;
  if (Storage.openFileForRead("RSS", "/apps/rss/font_size.txt", f)) {
    char c = f.available() > 0 ? f.read() : '0';
    f.close();
    uint8_t parsed = static_cast<uint8_t>(c - '0');
    if (parsed < kRssArticleFontCount) articleFontSizeIndex = parsed;
  }
}

void RssActivity::saveArticleFontSize() {
  HalFile f;
  if (Storage.openFileForWrite("RSS", "/apps/rss/font_size.txt", f)) {
    char c = static_cast<char>('0' + articleFontSizeIndex);
    f.write(&c, 1);
    f.close();
  }
}

bool RssActivity::parseFeedsFromMarkdown(const std::string& filepath, std::vector<RssItem>& targetList,
                                         bool summaryOnly) {
  HalFile file;
  if (!Storage.openFileForRead("RSS", filepath, file)) return false;

  std::string line;
  RssItem currentItem;
  bool inItem = false;

  auto handleLine = [&]() {
    if (line.rfind("## ", 0) == 0) {
      if (inItem && !currentItem.link.empty()) targetList.push_back(currentItem);
      currentItem = RssItem();
      currentItem.title = line.substr(3);
      inItem = true;
    } else if (inItem) {
      if (line.rfind("- Link: ", 0) == 0) {
        currentItem.link = line.substr(8);
      } else if (line.rfind("- Source: ", 0) == 0) {
        currentItem.feedName = line.substr(10);
      } else if (line.rfind("- Timestamp: ", 0) == 0) {
        currentItem.timestamp = line.substr(13);
      } else if (line.rfind("- Description: ", 0) == 0) {
        std::string rawDesc = unescapeNewlines(line.substr(15));
        if (summaryOnly && rawDesc.length() > 225) rawDesc = rawDesc.substr(0, 225) + "...";
        currentItem.description = rawDesc;
      } else if (line.rfind("- Content: ", 0) == 0) {
        if (!summaryOnly) currentItem.content = unescapeNewlines(line.substr(11));
      }
    }
  };

  while (file.available() > 0) {
    char c = file.read();
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      handleLine();
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) handleLine();

  if (inItem && !currentItem.link.empty()) targetList.push_back(currentItem);
  file.close();
  return true;
}

bool RssActivity::loadOfflineFeeds() {
  allItems.clear();
  std::string filename = getSanitizedUrlFilename(activeFeed);
  std::string filepath = "/apps/rss/" + filename + ".md";
  bool success = parseFeedsFromMarkdown(filepath, allItems, true);
  if (success) {
    std::sort(allItems.begin(), allItems.end(), [](const RssItem& a, const RssItem& b) {
      return atoll(a.timestamp.c_str()) > atoll(b.timestamp.c_str());
    });
  }
  return success && !allItems.empty();
}

void RssActivity::openFeed(const std::string& url) {
  activeFeed = url;
  bool hasCache = loadOfflineFeeds();
  selectedItemIndex = 0;
  itemsScrollOffset = 0;
  errorMessage.clear();
  if (hasCache) {
    state = RssState::FeedList;
    requestUpdate();
  } else {
    startFetch();
  }
}

void RssActivity::promptAddUrl() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RSS_ADD_URL), "https://", 150);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
    if (!keyboardResult || keyboardResult->text.empty() || keyboardResult->text == "https://") {
      requestUpdate();
      return;
    }
    const std::string url = keyboardResult->text;
    bool alreadySubscribed = false;
    for (const auto& sub : subscriptions) {
      if (sub.url == url) {
        alreadySubscribed = true;
        break;
      }
    }
    if (!alreadySubscribed) {
      subscriptions.push_back({url, ""});
      saveSubscriptions();
    }
    openFeed(url);
  });
}

void RssActivity::startFetch() {
  state = RssState::Loading;
  isRefreshing = true;
  // Free the resident item list before the TLS handshake: mbedTLS needs a
  // contiguous ~32KB (16KB in + 16KB out) buffer on this PSRAM-less chip, and
  // a cached list of up to 25 items is enough to fragment that away.
  allItems.clear();
  allItems.shrink_to_fit();
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this](const ActivityResult& result) {
                              if (result.isCancelled) {
                                isRefreshing = false;
                                loadOfflineFeeds();
                                if (allItems.empty()) errorMessage = tr(STR_RSS_WIFI_REQUIRED);
                                state = RssState::FeedList;
                                requestUpdate();
                              } else {
                                doFetch();
                              }
                            });
    return;
  }

  doFetch();
}

void RssActivity::doFetch() {
  requestUpdateAndWait();  // paint the "Loading"/"Refreshing" state before the blocking calls below

  wifiWasUsed = true;
  ensureDirectoriesExist();
  std::string xmlPath = "/apps/rss/temp.xml";
  Storage.remove(xmlPath.c_str());

  bool fetchSuccess = false;
  int retries = 3;
  while (retries-- > 0) {
    if (HttpDownloader::downloadToFile(activeFeed, xmlPath) == HttpDownloader::OK) {
      fetchSuccess = true;
      break;
    }
    if (retries > 0) delay(1500);
  }

  if (fetchSuccess) {
    std::string filename = getSanitizedUrlFilename(activeFeed);
    std::string mdPath = "/apps/rss/" + filename + ".md";
    parseXmlFile(xmlPath, mdPath, getFriendlyFeedName(activeFeed));
    Storage.remove(xmlPath.c_str());
  }

  isRefreshing = false;
  // The item list was cleared before the fetch started, so the reload must
  // happen unconditionally — on failure this is the only way to get the old
  // (still-good, untouched-on-disk) data back.
  loadOfflineFeeds();
  // fetchSuccess but still empty means the download itself was fine and the
  // problem is downstream -- the URL didn't point at readable RSS/Atom (wrong
  // path, an HTML page, a JSON API, ...). That's a different, actionable
  // problem from "no connection", so it gets its own message instead of the
  // same generic offline text regardless of what actually happened.
  if (allItems.empty()) {
    errorMessage = fetchSuccess ? tr(STR_RSS_UNREADABLE) : tr(STR_RSS_NO_DATA);
  } else {
    errorMessage.clear();
  }
  selectedItemIndex = 0;
  itemsScrollOffset = 0;
  state = RssState::FeedList;
  requestUpdate();
}

void RssActivity::showQrForActivePost() {
  if (selectedItemIndex < 0 || selectedItemIndex >= static_cast<int>(allItems.size())) return;
  const std::string url = allItems[selectedItemIndex].link;
  if (url.empty()) return;
  startActivityForResult(makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, url),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void RssActivity::onEnter() {
  Activity::onEnter();
  ensureDirectoriesExist();
  loadSubscriptions();
  loadArticleFontSize();

  state = RssState::FeedSelection;
  activeFeed.clear();
  selectedSubIndex = 0;
  isRefreshing = false;
  requestUpdate();
}

void RssActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void RssActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == RssState::Loading) return;  // owned by the blocking startFetch()/doFetch() call that triggered it

  if (mappedInput.wasReleased(Button::Back)) {
    if (state == RssState::FeedList) {
      activeFeed.clear();
      allItems.clear();
      state = RssState::FeedSelection;
      requestUpdate();
    } else if (state == RssState::FeedSelection) {
      onGoHome(HomeMenuItem::APPS_MENU);
    } else if (state == RssState::PostDetail) {
      loadOfflineFeeds();  // free RAM by reloading the summary-only feed list
      state = RssState::FeedList;
      requestUpdate();
    } else if (state == RssState::FeedActionMenu) {
      state = RssState::FeedSelection;
      requestUpdate();
    }
    return;
  }

  if (state == RssState::FeedSelection) {
    int totalItems = static_cast<int>(subscriptions.size()) + 1;  // subscriptions + "Add RSS URL"
    if (mappedInput.wasReleased(Button::Up)) {
      selectedSubIndex = (selectedSubIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      selectedSubIndex = (selectedSubIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (selectedSubIndex == totalItems - 1) {
        promptAddUrl();
      } else {
        openFeed(subscriptions[selectedSubIndex].url);
      }
    } else if (mappedInput.wasReleased(Button::Right)) {
      if (selectedSubIndex >= 0 && selectedSubIndex < totalItems - 1) {
        feedActionMenuIndex = 0;
        state = RssState::FeedActionMenu;
        requestUpdate();
      }
    }
    return;
  }

  if (state == RssState::FeedActionMenu) {
    constexpr int kFeedActionCount = 3;  // Edit Title, Edit URL, Delete Feed
    if (mappedInput.wasReleased(Button::Up)) {
      feedActionMenuIndex = (feedActionMenuIndex - 1 + kFeedActionCount) % kFeedActionCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      feedActionMenuIndex = (feedActionMenuIndex + 1) % kFeedActionCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (feedActionMenuIndex == 0) {
        // Edit Title
        const std::string currentUrl = subscriptions[selectedSubIndex].url;
        const std::string currentName = displayNameForUrl(currentUrl);
        auto keyboard =
            makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RSS_EDIT_TITLE), currentName, 60);
        startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
            if (keyboardResult && selectedSubIndex >= 0 &&
                selectedSubIndex < static_cast<int>(subscriptions.size()) && !keyboardResult->text.empty()) {
              const std::string autoName = getFriendlyFeedName(subscriptions[selectedSubIndex].url);
              subscriptions[selectedSubIndex].customName =
                  (keyboardResult->text == autoName) ? "" : keyboardResult->text;
              saveSubscriptions();
            }
          }
          state = RssState::FeedSelection;
          requestUpdate();
        });
      } else if (feedActionMenuIndex == 1) {
        // Edit URL
        std::string oldUrl = subscriptions[selectedSubIndex].url;
        auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RSS_EDIT_URL), oldUrl, 150);
        startActivityForResult(std::move(keyboard), [this, oldUrl](const ActivityResult& result) {
          if (!result.isCancelled) {
            auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
            bool urlTaken = false;
            if (keyboardResult) {
              for (const auto& sub : subscriptions) {
                if (sub.url == keyboardResult->text) {
                  urlTaken = true;
                  break;
                }
              }
            }
            if (keyboardResult && !keyboardResult->text.empty() && keyboardResult->text != oldUrl && !urlTaken) {
              for (auto& sub : subscriptions) {
                if (sub.url == oldUrl) {
                  sub.url = keyboardResult->text;
                  saveSubscriptions();
                  // Old cache is keyed off the old URL's filename hash; drop it so
                  // the edited URL fetches fresh content under its own cache file.
                  std::string oldFilename = getSanitizedUrlFilename(oldUrl);
                  Storage.remove(("/apps/rss/" + oldFilename + ".md").c_str());
                  if (activeFeed == oldUrl) activeFeed = keyboardResult->text;
                  break;
                }
              }
            }
          }
          state = RssState::FeedSelection;
          requestUpdate();
        });
      } else {
        // Delete Feed
        std::string subToDelete = subscriptions[selectedSubIndex].url;
        auto handler = [this, subToDelete](const ActivityResult& res) {
          if (!res.isCancelled) {
            for (size_t i = 0; i < subscriptions.size(); i++) {
              if (subscriptions[i].url == subToDelete) {
                subscriptions.erase(subscriptions.begin() + i);
                break;
              }
            }
            saveSubscriptions();
            selectedSubIndex = 0;
            std::string filename = getSanitizedUrlFilename(subToDelete);
            Storage.remove(("/apps/rss/" + filename + ".md").c_str());
          }
          state = RssState::FeedSelection;
          requestUpdate();
        };
        startActivityForResult(
            makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_RSS_UNSUBSCRIBE), subToDelete),
            handler);
      }
    }
    return;
  }

  if (state == RssState::FeedList) {
    if (mappedInput.wasReleased(Button::Right)) {
      startFetch();
      return;
    }
    if (allItems.empty()) {
      if (mappedInput.wasReleased(Button::Confirm)) startFetch();
      return;
    }
    if (mappedInput.wasReleased(Button::Up)) {
      if (selectedItemIndex > 0) {
        selectedItemIndex--;
        if (selectedItemIndex < itemsScrollOffset) itemsScrollOffset = selectedItemIndex;
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(Button::Down)) {
      if (selectedItemIndex < static_cast<int>(allItems.size()) - 1) {
        if (selectedItemIndex >= lastVisibleItemIndex) {
          // Already at the bottom of the current page -- jump the whole
          // window to the next unseen item instead of scrolling by one card,
          // so the page fully replaces and never re-shows an already-read item.
          itemsScrollOffset = selectedItemIndex + 1;
          selectedItemIndex = itemsScrollOffset;
        } else {
          selectedItemIndex++;
        }
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::Confirm)) {
      // Details are loaded on demand from the markdown file on disk (rather
      // than kept resident for every summary-only list item) to save RAM.
      const auto& item = allItems[selectedItemIndex];
      std::string filename = getSanitizedUrlFilename(activeFeed);
      std::string filepath = "/apps/rss/" + filename + ".md";
      loadSingleItemDetails(filepath, item.link, allItems[selectedItemIndex].description,
                            allItems[selectedItemIndex].content);

      state = RssState::PostDetail;
      detailScrollOffset = 0;
      requestUpdate();
    }
    return;
  }

  if (state == RssState::PostDetail) {
    if (mappedInput.wasReleased(Button::Up)) {
      if (detailScrollOffset > 0) {
        detailScrollOffset = std::max(0, detailScrollOffset - detailMaxLines);
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(Button::Down)) {
      // render() re-clamps this to the last valid page, so it's safe to
      // overshoot here — a full-page jump so the screen fully replaces
      // instead of shifting by a single row each press.
      detailScrollOffset += detailMaxLines;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      showQrForActivePost();
    } else if (mappedInput.wasReleased(Button::Left)) {
      if (articleFontSizeIndex > 0) {
        articleFontSizeIndex--;
        saveArticleFontSize();
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(Button::Right)) {
      if (articleFontSizeIndex + 1 < kRssArticleFontCount) {
        articleFontSizeIndex++;
        saveArticleFontSize();
        requestUpdate();
      }
    }
  }
}

void RssActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string headerTitle = tr(STR_RSS_TITLE);
  if (state == RssState::FeedList || state == RssState::Loading || state == RssState::PostDetail) {
    headerTitle = displayNameForUrl(activeFeed);
  }
  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  if (state == RssState::Loading) {
    int textY = contentTop + contentHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, isRefreshing ? tr(STR_RSS_REFRESHING) : tr(STR_RSS_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == RssState::FeedList) {
    if (!errorMessage.empty() && allItems.empty()) {
      // These messages got noticeably longer once they started saying *why*
      // the feed came up empty instead of one generic line -- drawCenteredText
      // is single-line only, so a long one ran off the screen edge.
      const int errWidth = pageWidth - 2 * metrics.contentSidePadding;
      auto errorLines = renderer.wrappedText(UI_10_FONT_ID, errorMessage.c_str(), errWidth, 4, EpdFontFamily::BOLD);
      const int errLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      int errY = contentTop + 40;
      for (const auto& line : errorLines) {
        renderer.drawCenteredText(UI_10_FONT_ID, errY, line.c_str(), true, EpdFontFamily::BOLD);
        errY += errLineHeight;
      }
    } else {
      const int cellX = metrics.contentSidePadding;
      const int cellW = pageWidth - 2 * metrics.contentSidePadding;
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      constexpr int kCardPadding = 12;  // top+bottom padding inside a card
      constexpr int kCardGap = 10;      // vertical gap between cards
      constexpr int kSectionGap = 6;
      constexpr int kMaxTitleLines = 3;
      constexpr int kMaxDescLines = 3;
      constexpr int kTimeReserve = 70;  // room for the relative-time label beside the title's first line

      // Cards are sized to their own wrapped-text line count (title, up to 3
      // lines, + description, up to 3) rather than a fixed height -- a fixed
      // height either clipped longer titles against the time label or wasted
      // space on short ones. Same approach as OnThisDayActivity's cards.
      auto cardHeightFor = [&](const RssItem& item) {
        auto titleLines = renderer.wrappedText(UI_10_FONT_ID, item.title.c_str(), cellW - 24 - kTimeReserve,
                                               kMaxTitleLines, EpdFontFamily::BOLD);
        int h = kCardPadding + static_cast<int>(titleLines.size()) * lineHeight;
        if (!item.description.empty()) {
          auto descLines = wrapParagraphs(renderer, UI_10_FONT_ID, item.description, cellW - 24, kMaxDescLines,
                                          EpdFontFamily::REGULAR);
          h += kSectionGap + static_cast<int>(descLines.size()) * lineHeight;
        }
        return h + kCardPadding + kCardGap;
      };

      // Grow itemsScrollOffset until the selected card is visible -- scrolling
      // upward (selectedItemIndex < itemsScrollOffset) is already snapped by
      // loop()'s Up handler; this only grows the window past the bottom, and
      // needs the real (variable) card heights computed above.
      if (selectedItemIndex >= itemsScrollOffset) {
        while (true) {
          int yy = contentTop;
          int lastVisible = itemsScrollOffset - 1;
          for (int idx = itemsScrollOffset; idx < static_cast<int>(allItems.size()); idx++) {
            const int h = cardHeightFor(allItems[idx]);
            if (yy + h > contentBottom) break;
            yy += h;
            lastVisible = idx;
          }
          if (selectedItemIndex <= lastVisible || itemsScrollOffset >= selectedItemIndex) break;
          itemsScrollOffset++;
        }
      }

      int cellY = contentTop;
      for (int idx = itemsScrollOffset; idx < static_cast<int>(allItems.size()); idx++) {
        const auto& item = allItems[idx];
        const int cardH = cardHeightFor(item);
        if (cellY + cardH > contentBottom) break;
        lastVisibleItemIndex = idx;

        bool isSelected = (idx == selectedItemIndex);
        renderer.drawRoundedRect(cellX, cellY, cellW, cardH - kCardGap, isSelected ? 3 : 1, 8, true);

        // The source is already shown once in the screen header (every card
        // here is from the same feed), so it isn't repeated per card. The
        // relative time sits beside the title's first line instead of on its
        // own row, with the title's wrap width narrowed by a fixed reserve so
        // it never collides.
        long long ts = atoll(item.timestamp.c_str());
        std::string relativeTime = timeAgo(static_cast<uint32_t>(ts));
        int textY = cellY + kCardPadding / 2;

        auto titleLines = renderer.wrappedText(UI_10_FONT_ID, item.title.c_str(), cellW - 24 - kTimeReserve,
                                               kMaxTitleLines, EpdFontFamily::BOLD);
        for (size_t l = 0; l < titleLines.size(); l++) {
          renderer.drawText(UI_10_FONT_ID, cellX + 12, textY, titleLines[l].c_str(), true, EpdFontFamily::BOLD);
          textY += lineHeight;
        }
        if (!relativeTime.empty()) {
          const int timeWidth = renderer.getTextWidth(UI_10_FONT_ID, relativeTime.c_str(), EpdFontFamily::REGULAR);
          renderer.drawText(UI_10_FONT_ID, cellX + cellW - 12 - timeWidth, cellY + kCardPadding / 2,
                            relativeTime.c_str(), true, EpdFontFamily::REGULAR);
        }
        textY += kSectionGap;

        if (!item.description.empty()) {
          auto descLines = wrapParagraphs(renderer, UI_10_FONT_ID, item.description, cellW - 24, kMaxDescLines,
                                          EpdFontFamily::REGULAR);
          for (const auto& descLine : descLines) {
            renderer.drawText(UI_10_FONT_ID, cellX + 12, textY, descLine.text.c_str(), true,
                              descLine.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
            textY += lineHeight;
          }
        }
        cellY += cardH;
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RSS_DETAILS), tr(STR_RSS_DETAILS), tr(STR_RSS_REFRESH));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == RssState::PostDetail) {
    if (selectedItemIndex < 0 || selectedItemIndex >= static_cast<int>(allItems.size())) {
      // Stale index (list shrunk from underneath this state) - bail back to
      // the feed list instead of an out-of-bounds vector access.
      state = RssState::FeedList;
      renderer.displayBuffer();
      return;
    }
    const auto& item = allItems[selectedItemIndex];

    const int articleFontId = kRssArticleFontIds[articleFontSizeIndex];
    const int lineHeight = renderer.getLineHeight(articleFontId);
    const int wrapWidth = pageWidth - 2 * metrics.contentSidePadding;

    // The title used to be drawn as its own fixed block capped at 3 lines,
    // pinned above the scrollable body -- a longer title just lost its
    // remaining lines with no way to read them. It's now the first (bold)
    // lines of the same scrollable list as the body, uncapped, so it scrolls
    // like everything else and nothing is ever truncated.
    auto titleLines = renderer.wrappedText(articleFontId, item.title.c_str(), wrapWidth, 500, EpdFontFamily::BOLD);

    std::string fullText = item.description;
    if (!item.content.empty()) {
      if (!fullText.empty()) fullText += "\n\n";
      fullText += item.content;
    }

    if (!item.link.empty()) {
      std::string formattedUrl;
      for (size_t i = 0; i < item.link.length(); ++i) {
        formattedUrl += item.link[i];
        if (item.link[i] == '?' || item.link[i] == '&' || item.link[i] == '-' || item.link[i] == '_') {
          formattedUrl += " ";
        } else if (item.link[i] == '/') {
          if (i + 1 < item.link.length() && item.link[i + 1] != '/') formattedUrl += " ";
        } else if (item.link[i] == '.') {
          if (i > 0 && !std::isdigit(static_cast<unsigned char>(item.link[i - 1]))) formattedUrl += " ";
        }
      }
      if (!fullText.empty()) fullText += "\n\n";
      fullText += std::string(tr(STR_RSS_LINK_LABEL)) + " " + formattedUrl;
    }

    auto bodyLines = wrapParagraphs(renderer, articleFontId, fullText, wrapWidth, 500, EpdFontFamily::REGULAR);

    // Title lines (bold) + a blank spacer + body lines (regular/bold per
    // heading), as one scrollable list so Up/Down page through title and
    // body together.
    std::vector<DetailLine> lines;
    lines.reserve(titleLines.size() + 1 + bodyLines.size());
    for (auto& l : titleLines) lines.push_back({l, true});
    lines.push_back({"", false});
    for (auto& l : bodyLines) lines.push_back(std::move(l));

    int maxLines = contentHeight / lineHeight;
    detailMaxLines = std::max(1, maxLines);
    if (detailScrollOffset > std::max(0, static_cast<int>(lines.size()) - maxLines)) {
      detailScrollOffset = std::max(0, static_cast<int>(lines.size()) - maxLines);
    }

    for (int i = 0; i < maxLines; i++) {
      int lineIdx = detailScrollOffset + i;
      if (lineIdx >= static_cast<int>(lines.size())) break;
      const auto& detailLine = lines[lineIdx];
      renderer.drawText(articleFontId, metrics.contentSidePadding, contentTop + i * lineHeight, detailLine.text.c_str(),
                        true, detailLine.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RSS_SHOW_QR), "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == RssState::FeedActionMenu) {
    const std::string feedTitle = (selectedSubIndex >= 0 && selectedSubIndex < static_cast<int>(subscriptions.size()))
                                       ? displayNameForUrl(subscriptions[selectedSubIndex].url)
                                       : tr(STR_RSS_TITLE);
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, feedTitle.c_str());

    constexpr int kFeedActionCount = 3;
    GUI.drawButtonMenu(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, kFeedActionCount, feedActionMenuIndex,
        [](int index) -> std::string {
          if (index == 0) return tr(STR_RSS_EDIT_TITLE);
          if (index == 1) return tr(STR_RSS_EDIT_URL);
          return tr(STR_RSS_DELETE_FEED);
        },
        [](int) { return UIIcon::Library; });

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == RssState::FeedSelection) {
    int totalItems = static_cast<int>(subscriptions.size()) + 1;
    GUI.drawButtonMenu(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedSubIndex,
        [this, totalItems](int index) {
          if (index == totalItems - 1) return std::string(tr(STR_RSS_ADD_URL_LABEL));
          const auto& sub = subscriptions[index];
          return !sub.customName.empty() ? sub.customName : getFriendlyFeedName(sub.url);
        },
        [this, totalItems](int index) { return index == totalItems - 1 ? UIIcon::File : UIIcon::Library; });

    const char* rightAction = (selectedSubIndex >= 0 && selectedSubIndex < totalItems - 1) ? tr(STR_RSS_MENU) : nullptr;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, rightAction);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

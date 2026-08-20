#include "HtmlTextCleanup.h"

#include <cctype>

namespace HtmlTextCleanup {

namespace {

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

}  // namespace

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
        if (isHeadingTag(lowerTagName) && !tagIsClosing) clean += kHeadingSentinel;
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

std::vector<DetailLine> wrapParagraphs(const GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth,
                                       int maxLines, EpdFontFamily::Style style) {
  std::vector<DetailLine> allLines;
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
      auto paragraphLines =
          renderer.wrappedText(fontId, paragraph.c_str(), maxWidth, budget, isHeading ? EpdFontFamily::BOLD : style);
      for (auto& line : paragraphLines) allLines.push_back({std::move(line), isHeading});
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return allLines;
}

}  // namespace HtmlTextCleanup

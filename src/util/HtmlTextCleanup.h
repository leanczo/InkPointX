#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

// Shared HTML/RSS text cleanup for any activity that renders freeform
// HTML-in-XML content (RSS descriptions, syndicated article bodies, etc.) as
// plain wrapped text on the e-ink screen. Originally lived only in
// RssActivity.cpp; extracted here once HoroscopoActivity needed the same
// HTML-stripping + paragraph-wrapping logic for its own RSS-sourced feed.
namespace HtmlTextCleanup {

// Marks the first character of a paragraph as a heading -- stripped back out
// (and turned into a bold line instead of just plain text) by wrapParagraphs()
// below. A raw SOH byte never occurs in real feed text.
constexpr char kHeadingSentinel = '\x01';

// Strips HTML tags, decodes entities, and collapses whitespace, while
// preserving paragraph breaks (as '\n') and marking headings (<h1>-<h6>) with
// kHeadingSentinel as the first character of their paragraph.
std::string cleanField(const std::string& input);

// A line of already-wrapped body text plus which font family it should draw
// with -- currently just heading (bold) vs. regular, since GfxRenderer only
// supports one style per drawText() call and has no inline mixed-run text.
struct DetailLine {
  std::string text;
  bool bold;
};

// wrappedText() wraps by width only and has no concept of '\n' as a line
// break -- it would render one embedded in the middle of a "word". This
// splits on the paragraph breaks cleanField() leaves behind and wraps each
// paragraph independently, passing each the line budget remaining out of
// maxLines so the overall cap (and wrappedText's own "..." truncation on the
// line that hits it) still applies across paragraphs, not just within one. A
// paragraph starting with kHeadingSentinel wraps in bold instead of `style`,
// with the sentinel itself stripped before it ever reaches
// wrappedText/drawText.
std::vector<DetailLine> wrapParagraphs(const GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth,
                                       int maxLines, EpdFontFamily::Style style);

}  // namespace HtmlTextCleanup

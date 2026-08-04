#pragma once

#include <cstdint>
#include <string>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

// Shared, low-overhead statistics collector for the non-reflow readers. EPUB
// has additional pace/finish integration of its own; TXT/Markdown and XTC use
// this helper so global statistics cover every book format InkPoint X opens.
class ReadingSessionStats {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingStatsDateTime sessionStart;
  uint32_t sessionSeconds = 0;
  unsigned long pageShownAtMs = 0;
  bool hasSessionStart = false;
  bool started = false;

  void collectCurrentPage(bool forwardPage);

 public:
  void begin(const std::string& cachePath);
  void pageShown();
  void pageTurn(bool forwardPage);
  void finish(const std::string& cachePath);
};

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

// Older firmware stored useful per-book statistics before it had a reliable
// global counter. Keep a compact floor built from the books we can still see so
// the new dashboard never says "0" while its own book list contains hours of
// reading. We deliberately take max(global, catalog) instead of adding them:
// current firmware writes both counters for the same session.
struct ReadingStatsBookTotals {
  uint32_t sessions = 0;
  uint32_t readingSeconds = 0;
  uint32_t pagesTurned = 0;
  uint32_t completedBooks = 0;
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
};

inline uint32_t addReadingStatsSaturated(const uint32_t left, const uint32_t right) {
  return std::numeric_limits<uint32_t>::max() - left < right ? std::numeric_limits<uint32_t>::max() : left + right;
}

inline void addBookToReadingStatsTotals(ReadingStatsBookTotals& totals, const BookReadingStats& book) {
  totals.sessions = addReadingStatsSaturated(totals.sessions, book.sessionCount);
  totals.readingSeconds = addReadingStatsSaturated(totals.readingSeconds, book.totalReadingSeconds);
  totals.pagesTurned = addReadingStatsSaturated(totals.pagesTurned, book.totalPagesTurned);
  if (book.isCompleted) totals.completedBooks = addReadingStatsSaturated(totals.completedBooks, 1);
  for (size_t i = 0; i < totals.timeOfDaySeconds.size(); ++i) {
    totals.timeOfDaySeconds[i] = addReadingStatsSaturated(totals.timeOfDaySeconds[i], book.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < totals.dayOfWeekSeconds.size(); ++i) {
    totals.dayOfWeekSeconds[i] = addReadingStatsSaturated(totals.dayOfWeekSeconds[i], book.dayOfWeekSeconds[i]);
  }
}

inline void applyBookTotalsFloor(GlobalReadingStats& stats, const ReadingStatsBookTotals& totals) {
  stats.totalSessions = std::max(stats.totalSessions, totals.sessions);
  stats.totalReadingSeconds = std::max(stats.totalReadingSeconds, totals.readingSeconds);
  stats.totalPagesTurned = std::max(stats.totalPagesTurned, totals.pagesTurned);
  stats.completedBooks = std::max(stats.completedBooks, totals.completedBooks);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    stats.timeOfDaySeconds[i] = std::max(stats.timeOfDaySeconds[i], totals.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    stats.dayOfWeekSeconds[i] = std::max(stats.dayOfWeekSeconds[i], totals.dayOfWeekSeconds[i]);
  }
}

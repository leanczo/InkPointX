#include <gtest/gtest.h>

#include <limits>

#include "ReadingStatsSummary.h"

TEST(ReadingStatsSummary, UsesPerBookHistoryWhenGlobalStatsAreEmpty) {
  BookReadingStats book;
  book.sessionCount = 3;
  book.totalReadingSeconds = 5400;
  book.totalPagesTurned = 80;
  book.isCompleted = true;
  book.timeOfDaySeconds[2] = 4000;
  book.dayOfWeekSeconds[1] = 3000;

  ReadingStatsBookTotals totals;
  addBookToReadingStatsTotals(totals, book);
  GlobalReadingStats global;
  applyBookTotalsFloor(global, totals);

  EXPECT_EQ(global.totalSessions, 3u);
  EXPECT_EQ(global.totalReadingSeconds, 5400u);
  EXPECT_EQ(global.totalPagesTurned, 80u);
  EXPECT_EQ(global.completedBooks, 1u);
  EXPECT_EQ(global.timeOfDaySeconds[2], 4000u);
  EXPECT_EQ(global.dayOfWeekSeconds[1], 3000u);
}

TEST(ReadingStatsSummary, DoesNotDoubleCountSessionsAlreadyPresentGlobally) {
  BookReadingStats book;
  book.sessionCount = 3;
  book.totalReadingSeconds = 5400;

  ReadingStatsBookTotals totals;
  addBookToReadingStatsTotals(totals, book);
  GlobalReadingStats global;
  global.totalSessions = 5;
  global.totalReadingSeconds = 7200;
  applyBookTotalsFloor(global, totals);

  EXPECT_EQ(global.totalSessions, 5u);
  EXPECT_EQ(global.totalReadingSeconds, 7200u);
}

TEST(ReadingStatsSummary, SaturatesCatalogTotals) {
  ReadingStatsBookTotals totals;
  totals.readingSeconds = std::numeric_limits<uint32_t>::max() - 10;
  BookReadingStats book;
  book.totalReadingSeconds = 100;
  addBookToReadingStatsTotals(totals, book);
  EXPECT_EQ(totals.readingSeconds, std::numeric_limits<uint32_t>::max());
}

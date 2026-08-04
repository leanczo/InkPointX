#include <gtest/gtest.h>

#include "ReadingHistoryTimeline.h"

TEST(ReadingHistoryTimeline, ShiftsNewDaysWithoutLosingOlderTotals) {
  DailyReadingTimeline values{};
  values[0] = 120;
  values[1] = 60;
  shiftDailyReadingTimeline(values, 2);
  EXPECT_EQ(values[0], 0);
  EXPECT_EQ(values[1], 0);
  EXPECT_EQ(values[2], 120);
  EXPECT_EQ(values[3], 60);
}

TEST(ReadingHistoryTimeline, SaturatesAnExtremeSingleDay) {
  DailyReadingTimeline values{};
  addDailyReadingSeconds(1000, values, 1000, 65000);
  addDailyReadingSeconds(1000, values, 1000, 1000);
  EXPECT_EQ(values[0], UINT16_MAX);
}

TEST(ReadingHistoryTimeline, MergesDifferentAnchorsByCalendarDay) {
  uint32_t targetAnchor = 100;
  DailyReadingTimeline target{};
  target[0] = 10;  // day 100
  target[2] = 30;  // day 98
  DailyReadingTimeline source{};
  source[0] = 40;  // day 102
  source[2] = 50;  // day 100

  mergeDailyReadingTimeline(targetAnchor, target, 102, source);

  EXPECT_EQ(targetAnchor, 102u);
  EXPECT_EQ(target[0], 40);
  EXPECT_EQ(target[2], 60);
  EXPECT_EQ(target[4], 30);
}

TEST(ReadingHistoryTimeline, IgnoresDatesOutsideRollingWindow) {
  DailyReadingTimeline values{};
  addDailyReadingSeconds(1000, values, 1000 - READING_HISTORY_DAYS, 42);
  EXPECT_EQ(dailyReadingSecondsForDay(1000, values, 1000 - READING_HISTORY_DAYS), 0u);
}

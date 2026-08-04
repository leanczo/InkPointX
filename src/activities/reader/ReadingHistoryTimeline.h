#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ReadingStatsUtils.h"

using DailyReadingTimeline = std::array<uint16_t, READING_HISTORY_DAYS>;

inline void shiftDailyReadingTimeline(DailyReadingTimeline& values, const uint32_t shiftDays) {
  if (shiftDays == 0) return;
  if (shiftDays >= values.size()) {
    values.fill(0);
    return;
  }
  for (size_t i = values.size(); i-- > shiftDays;) values[i] = values[i - shiftDays];
  std::fill_n(values.begin(), shiftDays, 0);
}

inline uint16_t addDailyReadingSecondsSaturated(const uint16_t current, const uint32_t seconds) {
  constexpr uint16_t MAX = std::numeric_limits<uint16_t>::max();
  return seconds >= MAX || current > MAX - seconds ? MAX : static_cast<uint16_t>(current + seconds);
}

inline void addDailyReadingSeconds(const uint32_t anchorDay, DailyReadingTimeline& values, const uint32_t day,
                                   const uint32_t seconds) {
  if (anchorDay == 0 || day > anchorDay || seconds == 0) return;
  const uint32_t index = anchorDay - day;
  if (index < values.size()) values[index] = addDailyReadingSecondsSaturated(values[index], seconds);
}

inline uint32_t dailyReadingSecondsForDay(const uint32_t anchorDay, const DailyReadingTimeline& values,
                                          const uint32_t day) {
  if (anchorDay == 0 || day > anchorDay) return 0;
  const uint32_t index = anchorDay - day;
  return index < values.size() ? values[index] : 0;
}

inline void mergeDailyReadingTimeline(uint32_t& targetAnchor, DailyReadingTimeline& target, const uint32_t sourceAnchor,
                                      const DailyReadingTimeline& source) {
  if (sourceAnchor == 0) return;
  const uint32_t mergedAnchor = std::max(targetAnchor, sourceAnchor);
  if (targetAnchor != 0 && mergedAnchor > targetAnchor) shiftDailyReadingTimeline(target, mergedAnchor - targetAnchor);
  for (size_t sourceIndex = 0; sourceIndex < source.size(); ++sourceIndex) {
    const uint16_t value = source[sourceIndex];
    if (value == 0 || sourceIndex > sourceAnchor) continue;
    const uint32_t sourceDay = sourceAnchor - static_cast<uint32_t>(sourceIndex);
    addDailyReadingSeconds(mergedAnchor, target, sourceDay, value);
  }
  targetAnchor = mergedAnchor;
}

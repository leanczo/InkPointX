#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"

class HomeActivity final : public Activity {
  static constexpr int PAGE_COUNT = 3;

  struct ReadingSummary {
    uint8_t progressPercent = 0;
    uint32_t readingSeconds = 0;
    uint32_t currentPage = 0;
    uint32_t totalPages = 0;
  };

  int pageIndex = 0;
  int selectedIndex = 0;
  // Where the selection was on each page. Resetting to the first item on every
  // page change meant stepping over to Now Reading and back lost your place in a
  // seven-item list, which makes the carousel feel lossy to move through.
  static constexpr int PAGE_COUNT_MAX = 3;
  int rememberedSelection[PAGE_COUNT_MAX] = {0, 0, 0};
  std::vector<RecentBook> recentBooks;
  ReadingSummary readingSummary;
  std::string homeCoverPath;
  bool recentDetailsLoaded = false;
  const HomeMenuItem initialMenuItem;

  void applyInitialSelection();
  void loadRecentBookDetails();
  int pageItemCount() const;
  void openSelection();

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

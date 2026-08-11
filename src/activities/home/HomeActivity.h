#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"

class HomeActivity final : public Activity {
  static constexpr int PAGE_COUNT = 6;

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
  static constexpr int PAGE_COUNT_MAX = 6;
  int rememberedSelection[PAGE_COUNT_MAX] = {0, 0, 0, 0, 0, 0};
  std::vector<RecentBook> recentBooks;
  ReadingSummary readingSummary;
  std::string homeCoverPath;
  std::string homeCachePath;
  std::unique_ptr<uint8_t[]> coverRegionCache;
  size_t coverRegionCacheSize = 0;
  int coverRegionX = 0;
  int coverRegionY = 0;
  int coverRegionWidth = 0;
  int coverRegionHeight = 0;
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

  // Reader/cache mutations call this so the next Home instance reloads the
  // tiny progress record while ordinary menu round-trips stay instant.
  static void invalidateDetailsCache();

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

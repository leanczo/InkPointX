#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const uint8_t currentPageTurnOption, const bool hasFootnotes,
                                               const bool hasBookmarks, const bool isFavorite, const bool isPdf,
                                               const uint8_t currentPdfZoomOption)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, hasBookmarks, isFavorite, isPdf)),
      menuRows(buildMenuRows(menuItems)),
      title(title),
      pendingOrientation(currentOrientation),
      selectedPageTurnOption(currentPageTurnOption),
      selectedPdfZoomOption(std::min<uint8_t>(currentPdfZoomOption, std::size(pdfZoomLabels) - 1)),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes,
                                                                                     bool hasBookmarks, bool isFavorite,
                                                                                     bool isPdf) {
  std::vector<MenuItem> items;
  items.reserve(20);

  // Navigation stays together and follows the order readers normally use it:
  // structure first, then precise locations, then reference tools.
  items.push_back(
      {MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER, MenuGroup::Navigation, UIIcon::ReaderChapters});
  items.push_back({MenuAction::GO_TO_PAGE, StrId::STR_GO_TO_PAGE, MenuGroup::Navigation, UIIcon::ReaderPage});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT, MenuGroup::Navigation, UIIcon::ReaderStats});
  items.push_back({MenuAction::DICTIONARY, StrId::STR_DICTIONARY, MenuGroup::Navigation, UIIcon::ReaderDictionary});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES, MenuGroup::Navigation, UIIcon::ReaderFootnotes});
  }

  // Book-specific actions are separate from reading behaviour. This also puts
  // the one-tap bookmark action directly above the saved-bookmarks screen.
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK, MenuGroup::Book, UIIcon::Bookmark});
  // Gated like Footnotes: the bookmarks screen renders nothing but a header when
  // there are none, so offering it was a dead end. Toggle Bookmark below still
  // creates the first one.
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS, MenuGroup::Book, UIIcon::Bookmark});
  }
  items.push_back({MenuAction::TOGGLE_FAVORITE,
                   isFavorite ? StrId::STR_REMOVE_FROM_FAVORITES : StrId::STR_ADD_TO_FAVORITES, MenuGroup::Book,
                   UIIcon::Favorite});
  items.push_back({MenuAction::BOOK_INFO, StrId::STR_BOOK_INFO, MenuGroup::Book, UIIcon::System});
  items.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS, MenuGroup::Book, UIIcon::ReaderStats});

  items.push_back({MenuAction::READING_SETTINGS, StrId::STR_READING_SETTINGS, MenuGroup::Reading, UIIcon::Reading});
  if (isPdf) {
    items.push_back({MenuAction::PDF_ZOOM, StrId::STR_PDF_ZOOM, MenuGroup::Reading, UIIcon::Image});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION, MenuGroup::Reading, UIIcon::ReaderRotate});
  items.push_back(
      {MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN, MenuGroup::Reading, UIIcon::ReaderAutoTurn});
  // What the buttons do. The reading page shows no legend by design, so this is the
  // only place the hold gestures are stated.
  items.push_back({MenuAction::GESTURES, StrId::STR_SETTINGS_CONTROLS, MenuGroup::Reading, UIIcon::Controls});

  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS, MenuGroup::More, UIIcon::NetworkSync});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR, MenuGroup::More, UIIcon::ReaderQr});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON, MenuGroup::More, UIIcon::Image});
  items.push_back({MenuAction::OPEN_FROM_FILE, StrId::STR_OPEN_FROM_FILE, MenuGroup::More, UIIcon::Files});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON, MenuGroup::More, UIIcon::ReaderHome});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE, MenuGroup::More, UIIcon::ReaderTrash});
  return items;
}

std::vector<EpubReaderMenuActivity::MenuRow> EpubReaderMenuActivity::buildMenuRows(const std::vector<MenuItem>& items) {
  std::vector<MenuRow> rows;
  rows.reserve(items.size() + 4);
  MenuGroup previousGroup = MenuGroup::More;
  bool first = true;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    if (first || items[i].group != previousGroup) {
      rows.push_back({true, items[i].group, -1});
      previousGroup = items[i].group;
      first = false;
    }
    rows.push_back({false, items[i].group, i});
  }
  return rows;
}

StrId EpubReaderMenuActivity::groupLabelId(const MenuGroup group) {
  switch (group) {
    case MenuGroup::Navigation:
      return StrId::STR_NAVIGATION;
    case MenuGroup::Book:
      return StrId::STR_BOOK;
    case MenuGroup::Reading:
      return StrId::STR_SETTINGS_READING;
    case MenuGroup::More:
      return StrId::STR_MORE;
  }
  return StrId::STR_MORE;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      selectedPageTurnOption = (selectedPageTurnOption + 1) % pageTurnLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::PDF_ZOOM) {
      selectedPdfZoomOption = (selectedPdfZoomOption + 1) % std::size(pdfZoomLabels);
      requestUpdate();
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption,
                         selectedPdfZoomOption});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption, selectedPdfZoomOption};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // contentTop already includes screen.y, so the remaining height is measured from
  // the safe area's bottom edge. Subtracting it from screen.height instead lost
  // screen.y worth of list rows in Portrait-Inverted.
  const int contentHeight =
      (screen.y + screen.height) - contentTop - metrics.verticalSpacing - BaseTheme::footerCounterTopOffset;

  int selectedRowIndex = 0;
  for (int i = 0; i < static_cast<int>(menuRows.size()); ++i) {
    if (!menuRows[i].isSection && menuRows[i].itemIndex == selectedIndex) {
      selectedRowIndex = i;
      break;
    }
  }

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuRows.size(), selectedRowIndex,
      [this](int index) {
        const auto& row = menuRows[index];
        return row.isSection ? I18N.get(groupLabelId(row.group)) : I18N.get(menuItems[row.itemIndex].labelId);
      },
      nullptr,
      [this](int index) {
        const auto& row = menuRows[index];
        return row.isSection ? UIIcon::None : menuItems[row.itemIndex].icon;
      },
      [this](int index) {
        const auto& row = menuRows[index];
        if (row.isSection) return "";
        const auto value = menuItems[row.itemIndex].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          // Render current orientation value on the right edge of the content area.
          return I18N.get(orientationLabels[pendingOrientation]);
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          // Render current page turn value on the right edge of the content area.
          return pageTurnLabels[selectedPageTurnOption];
        } else if (value == MenuAction::PDF_ZOOM) {
          return pdfZoomLabels[selectedPdfZoomOption];
        } else {
          return "";
        }
      },
      true, nullptr,
      [this](int index) {
        const auto& row = menuRows[index];
        if (row.isSection) return UIAccessory::None;
        const auto action = menuItems[row.itemIndex].action;
        if (action == MenuAction::ROTATE_SCREEN || action == MenuAction::AUTO_PAGE_TURN ||
            action == MenuAction::PDF_ZOOM || action == MenuAction::TOGGLE_BOOKMARK ||
            action == MenuAction::TOGGLE_FAVORITE || action == MenuAction::SYNC || action == MenuAction::SCREENSHOT ||
            action == MenuAction::GO_HOME || action == MenuAction::DELETE_CACHE) {
          return UIAccessory::None;
        }
        return UIAccessory::Chevron;
      },
      [this](int index) { return menuRows[index].isSection; });

  // Footer / Hints
  GUI.drawFooterCounter(renderer, selectedIndex, static_cast<int>(menuItems.size()), progressLine.c_str());
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

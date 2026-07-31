#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "BidiUtils.h"
#include "MappedInputManager.h"
#include "activities/network/NetworkModeSelectionActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "components/UITheme.h"
#include "components/icons/lucide_ui.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr int HOME_CONTENT_MARGIN = 18;
constexpr int HOME_COVER_TOP = 88;
constexpr int HOME_COVER_SOURCE_HEIGHT = 402;
constexpr int HOME_COVER_MAX_WIDTH = 280;
constexpr int HOME_COVER_MIN_HEIGHT = 160;
constexpr int HOME_COVER_RADIUS = 14;
constexpr int HOME_COVER_TO_TITLE_GAP = 12;
constexpr int HOME_TITLE_TO_AUTHOR_GAP = 3;
constexpr int HOME_METADATA_GAP = 8;
constexpr int HOME_PROGRESS_BAR_GAP = 6;
constexpr int HOME_PROGRESS_BAR_THICKNESS = 2;
constexpr int HOME_PROGRESS_TO_TIME_GAP = 11;
constexpr int HOME_TIME_TO_ACTION_GAP = 12;
constexpr int HOME_ACTION_SIDE_MARGIN = 20;
constexpr int HOME_CONTINUE_HEIGHT = 66;
constexpr int HOME_DOTS_TOP_OFFSET = 50;
constexpr int HOME_DOTS_CLEARANCE = 22;
constexpr int HOME_ACTION_EDGE_PADDING = 18;
constexpr int HOME_ACTION_ICON_GAP = 12;

void drawHomeActionRow(const GfxRenderer& renderer, const Rect rect, const char* label) {
  GUI.drawSelection(renderer, rect);

  constexpr int leadingIconSize = 32;
  constexpr int accessorySize = 24;
  const bool rtl = BidiUtils::startsWithRtl(label);
  const int centerY = rect.y + rect.height / 2;
  const int leadingIconX =
      rtl ? rect.x + rect.width - HOME_ACTION_EDGE_PADDING - leadingIconSize : rect.x + HOME_ACTION_EDGE_PADDING;
  const int accessoryX =
      rtl ? rect.x + HOME_ACTION_EDGE_PADDING : rect.x + rect.width - HOME_ACTION_EDGE_PADDING - accessorySize;
  const int textLeft = rtl ? accessoryX + accessorySize + HOME_ACTION_ICON_GAP
                           : leadingIconX + leadingIconSize + HOME_ACTION_ICON_GAP;
  const int textRight = rtl ? leadingIconX - HOME_ACTION_ICON_GAP : accessoryX - HOME_ACTION_ICON_GAP;
  const int textWidth = std::max(0, textRight - textLeft);
  const auto text = renderer.truncatedText(UI_10_FONT_ID, label, textWidth, EpdFontFamily::BOLD);
  const int renderedTextWidth = renderer.getTextWidth(UI_10_FONT_ID, text.c_str(), EpdFontFamily::BOLD);
  const int textX = rtl ? textRight - renderedTextWidth : textLeft;
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

  renderer.drawIcon(LucideBookOpen32, leadingIconX, centerY - leadingIconSize / 2, leadingIconSize, leadingIconSize);
  renderer.drawText(UI_10_FONT_ID, textX, textY, text.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawIcon(rtl ? LucideChevronLeft24 : LucideChevronRight24, accessoryX, centerY - accessorySize / 2,
                    accessorySize, accessorySize);
}

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t clampPageCount(const double value) {
  if (value <= 0.0) return 0;
  const double maximum = static_cast<double>(std::numeric_limits<uint32_t>::max());
  return static_cast<uint32_t>(std::min(value, maximum));
}

bool isUsableBitmap(const std::string& path) {
  if (!Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("HOME", path, file)) return false;
  Bitmap bitmap(file);
  const bool usable = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0;
  file.close();
  return usable;
}

bool prepareHomeThumb(const Epub& epub, const std::string& path, const int targetHeight) {
  if (isUsableBitmap(path)) return true;
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  if (!epub.generateThumbBmp(targetHeight)) return false;
  return isUsableBitmap(path);
}

std::string filenameWithoutExtension(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  const size_t length = dot == std::string::npos || dot <= start ? std::string::npos : dot - start;
  return path.substr(start, length);
}

int calculateHomeCoverSlotHeight(const GfxRenderer& renderer, const int titleLineCount, const bool hasAuthor) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int titleBlockHeight = std::max(1, titleLineCount) * renderer.getLineHeight(UI_14_FONT_ID);
  const int metadataLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int authorBlockHeight = hasAuthor ? HOME_TITLE_TO_AUTHOR_GAP + metadataLineHeight : 0;
  const int detailTailHeight =
      HOME_COVER_TO_TITLE_GAP + titleBlockHeight + authorBlockHeight + HOME_METADATA_GAP + metadataLineHeight +
      HOME_PROGRESS_BAR_GAP + HOME_PROGRESS_BAR_THICKNESS + HOME_PROGRESS_TO_TIME_GAP + metadataLineHeight +
      HOME_TIME_TO_ACTION_GAP + HOME_CONTINUE_HEIGHT;
  const int dotsY = renderer.getScreenHeight() - metrics.buttonHintsHeight - HOME_DOTS_TOP_OFFSET;
  const int safeDetailsBottom = dotsY - HOME_DOTS_CLEARANCE;
  return std::max(HOME_COVER_MIN_HEIGHT,
                  std::min(HOME_COVER_SOURCE_HEIGHT, safeDetailsBottom - HOME_COVER_TOP - detailTailHeight));
}

}  // namespace

void HomeActivity::onEnter() {
  Activity::onEnter();
  recentBooks.clear();
  recentBooks.reserve(1);
  const auto& books = RECENT_BOOKS.getBooks();
  const auto availableBook = std::find_if(books.begin(), books.end(),
                                          [](const RecentBook& book) { return !RecentBooksStore::isMissing(book); });
  if (availableBook != books.end()) {
    recentBooks.push_back(*availableBook);
  }
  applyInitialSelection();
  if (pageIndex == 0) loadRecentBookDetails();
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  homeCoverPath.clear();
  readingSummary = {};
  recentDetailsLoaded = false;
}

void HomeActivity::loadRecentBookDetails() {
  recentDetailsLoaded = true;
  readingSummary = {};
  homeCoverPath.clear();
  if (recentBooks.empty()) return;

  const RecentBook& book = recentBooks.front();
  const std::string displayTitle = book.title.empty() ? filenameWithoutExtension(book.path) : book.title;
  const int titleLineCount = static_cast<int>(
      renderer.wrappedText(UI_14_FONT_ID, displayTitle.c_str(),
                           renderer.getScreenWidth() - HOME_CONTENT_MARGIN * 2, 2, EpdFontFamily::BOLD)
          .size());
  const int coverTargetHeight = calculateHomeCoverSlotHeight(renderer, titleLineCount, !book.author.empty());
  const bool epubCompatible = FsHelpers::hasEpubExtension(book.path) || FsHelpers::hasFb2Extension(book.path) ||
                              FsHelpers::hasPdfExtension(book.path);

  std::string cachePath = getBookCachePath(book.path);
  Epub epub(book.path, "/.crosspoint");
  const bool metadataLoaded = epubCompatible && epub.load(false, true);
  if (epubCompatible) cachePath = epub.getCachePath();

  if (metadataLoaded) {
    const std::string requestedThumb = epub.getThumbBmpPath(coverTargetHeight);
    if (prepareHomeThumb(epub, requestedThumb, coverTargetHeight)) {
      homeCoverPath = requestedThumb;
    } else {
      const std::array<std::string, 4> fallbackCovers = {
          epub.getCoverBmpPath(false), epub.getThumbBmpPath(HOME_COVER_SOURCE_HEIGHT), epub.getThumbBmpPath(300),
          epub.getThumbBmpPath(226)};
      const auto fallback = std::find_if(fallbackCovers.begin(), fallbackCovers.end(),
                                         [](const std::string& path) { return isUsableBitmap(path); });
      if (fallback != fallbackCovers.end()) homeCoverPath = *fallback;
    }
  } else if (!book.coverBmpPath.empty()) {
    const std::array<int, 4> fallbackHeights = {coverTargetHeight, HOME_COVER_SOURCE_HEIGHT, 300, 226};
    for (const int height : fallbackHeights) {
      const std::string candidate = UITheme::getCoverThumbPath(book.coverBmpPath, height);
      if (isUsableBitmap(candidate)) {
        homeCoverPath = candidate;
        break;
      }
    }
  }

  if (cachePath.empty()) return;
  const BookReadingStats stats = BookReadingStats::load(cachePath);
  readingSummary.readingSeconds = stats.totalReadingSeconds;

  if (!epubCompatible) {
    readingSummary.currentPage = stats.totalPagesTurned;
    return;
  }

  HalFile progressFile;
  if (!Storage.openFileForRead("HOME", cachePath + "/progress.bin", progressFile)) return;
  uint8_t data[6] = {};
  const int bytesRead = progressFile.read(data, sizeof(data));
  progressFile.close();
  if (bytesRead != 4 && bytesRead != 6) return;

  const uint16_t spineIndex = readLe16(data);
  uint16_t chapterPage = readLe16(data + 2);
  if (chapterPage == UINT16_MAX) chapterPage = 0;
  const uint16_t chapterPageCount = bytesRead == 6 ? readLe16(data + 4) : 0;
  if (!metadataLoaded || spineIndex >= epub.getSpineItemsCount()) return;

  const float sectionRead =
      chapterPageCount > 0
          ? std::min(1.0f, static_cast<float>(std::min<uint16_t>(chapterPage + 1, chapterPageCount)) / chapterPageCount)
          : 0.0f;
  const float bookProgress = std::clamp(epub.calculateProgress(spineIndex, sectionRead), 0.0f, 1.0f);
  readingSummary.progressPercent = static_cast<uint8_t>(bookProgress * 100.0f + 0.5f);

  if (chapterPageCount == 0) return;
  const size_t previousBytes = spineIndex > 0 ? epub.getCumulativeSpineItemSize(spineIndex - 1) : 0;
  const size_t cumulativeBytes = epub.getCumulativeSpineItemSize(spineIndex);
  const size_t chapterBytes = cumulativeBytes > previousBytes ? cumulativeBytes - previousBytes : 0;
  const size_t bookBytes = epub.getBookSize();
  if (chapterBytes == 0 || bookBytes == 0) return;

  const double bytesPerPage = static_cast<double>(chapterBytes) / chapterPageCount;
  readingSummary.totalPages = clampPageCount(std::ceil(static_cast<double>(bookBytes) / bytesPerPage));
  const double readBytes = static_cast<double>(previousBytes) + static_cast<double>(chapterBytes) * sectionRead;
  readingSummary.currentPage = clampPageCount(std::round(readBytes / bytesPerPage));
  if (readingSummary.totalPages > 0) {
    readingSummary.currentPage = std::clamp<uint32_t>(readingSummary.currentPage, 1, readingSummary.totalPages);
  }
}

void HomeActivity::applyInitialSelection() {
  pageIndex = 0;
  selectedIndex = 0;
  switch (initialMenuItem) {
    case HomeMenuItem::LIBRARY:
      pageIndex = 1;
      selectedIndex = 0;
      break;
    case HomeMenuItem::FILE_BROWSER:
      pageIndex = 1;
      selectedIndex = 1;
      break;
    case HomeMenuItem::GALLERY:
      pageIndex = 1;
      selectedIndex = 2;
      break;
    case HomeMenuItem::FAVORITES:
      pageIndex = 1;
      selectedIndex = 3;
      break;
    case HomeMenuItem::FILE_TRANSFER:
      pageIndex = 1;
      selectedIndex = 4;
      break;
    case HomeMenuItem::OPDS_BROWSER:
      pageIndex = 2;
      selectedIndex = 5;
      break;
    case HomeMenuItem::SETTINGS_MENU:
      pageIndex = 2;
      selectedIndex = 0;
      break;
    case HomeMenuItem::SETTINGS_POWER:
      pageIndex = 2;
      selectedIndex = 1;
      break;
    case HomeMenuItem::SETTINGS_READING:
      pageIndex = 2;
      selectedIndex = 2;
      break;
    case HomeMenuItem::SETTINGS_CONTROLS:
      pageIndex = 2;
      selectedIndex = 3;
      break;
    case HomeMenuItem::SETTINGS_LIBRARY:
      pageIndex = 2;
      selectedIndex = 4;
      break;
    case HomeMenuItem::SETTINGS_NETWORK:
      pageIndex = 2;
      selectedIndex = 5;
      break;
    case HomeMenuItem::SETTINGS_SYSTEM:
      pageIndex = 2;
      selectedIndex = 6;
      break;
    case HomeMenuItem::RECENTS:
    case HomeMenuItem::NONE:
      break;
  }
}

int HomeActivity::pageItemCount() const {
  if (pageIndex == 0) return 1;
  if (pageIndex == 1) return 7;
  return SettingsActivity::CATEGORY_COUNT;
}

void HomeActivity::openSelection() {
  if (pageIndex == 0) {
    if (!recentBooks.empty()) {
      activityManager.goToReader(recentBooks[0].path);
    } else {
      activityManager.goToLibrary();
    }
    return;
  }

  if (pageIndex == 1) {
    switch (selectedIndex) {
      case 0:
        activityManager.goToLibrary();
        return;
      case 1:
        activityManager.goToFileBrowser();
        return;
      case 2:
        activityManager.goToGallery();
        return;
      case 3:
        activityManager.goToFavorites();
        return;
      case 4:
        activityManager.goToFileTransfer(NetworkMode::JOIN_NETWORK);
        return;
      case 5:
        activityManager.goToFileTransfer(NetworkMode::CONNECT_CALIBRE);
        return;
      case 6:
        activityManager.goToFileTransfer(NetworkMode::CREATE_HOTSPOT);
        return;
      default:
        return;
    }
  }

  activityManager.goToSettings(selectedIndex);
}

void HomeActivity::loop() {
  const auto changePage = [this](const int delta) {
    rememberedSelection[pageIndex] = selectedIndex;
    pageIndex = (pageIndex + delta + PAGE_COUNT) % PAGE_COUNT;
    // Clamp on the way in: a page's item count can change between visits.
    selectedIndex = std::min(rememberedSelection[pageIndex], std::max(0, pageItemCount() - 1));
    if (pageIndex == 0 && !recentDetailsLoaded) loadRecentBookDetails();
    requestUpdate();
  };

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    changePage(1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    changePage(-1);
    return;
  }

  const int count = pageItemCount();
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex + 1) % count;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex + count - 1) % count;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) openSelection();
}

void HomeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (pageIndex == 0) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NOW_READING));

    const bool hasRecentBook = !recentBooks.empty();
    const RecentBook* recentBook = hasRecentBook ? &recentBooks.front() : nullptr;
    const std::string displayTitle =
        hasRecentBook
            ? (recentBook->title.empty() ? filenameWithoutExtension(recentBook->path) : recentBook->title)
            : std::string(tr(STR_NO_OPEN_BOOK));
    const int textWidth = pageWidth - HOME_CONTENT_MARGIN * 2;
    const auto titleLines =
        renderer.wrappedText(UI_14_FONT_ID, displayTitle.c_str(), textWidth, 2, EpdFontFamily::BOLD);
    const bool hasAuthor = hasRecentBook ? !recentBook->author.empty() : true;
    const int titleLineHeight = renderer.getLineHeight(UI_14_FONT_ID);
    const int metadataLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int titleBlockHeight = std::max(1, static_cast<int>(titleLines.size())) * titleLineHeight;
    const int coverSlotHeight =
        calculateHomeCoverSlotHeight(renderer, static_cast<int>(titleLines.size()), hasAuthor);

    bool coverDrawn = false;
    if (!recentBooks.empty() && !homeCoverPath.empty()) {
      HalFile coverFile;
      if (Storage.openFileForRead("HOME", homeCoverPath, coverFile)) {
        Bitmap bitmap(coverFile);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
          const float scale = std::min(static_cast<float>(HOME_COVER_MAX_WIDTH) / bitmap.getWidth(),
                                       static_cast<float>(coverSlotHeight) / bitmap.getHeight());
          const int coverWidth = std::max(1, static_cast<int>(bitmap.getWidth() * scale));
          const int coverHeight = std::max(1, static_cast<int>(bitmap.getHeight() * scale));
          const int coverX = (pageWidth - coverWidth) / 2;
          const int coverY = HOME_COVER_TOP + (coverSlotHeight - coverHeight) / 2;
          renderer.drawBitmap(bitmap, coverX, coverY, coverWidth, coverHeight);
          renderer.maskRoundedRectOutsideCorners(coverX, coverY, coverWidth, coverHeight, HOME_COVER_RADIUS,
                                                 Color::White);
          renderer.drawRoundedRect(coverX, coverY, coverWidth, coverHeight, 1, HOME_COVER_RADIUS, true);
          coverDrawn = true;
        }
        coverFile.close();
      }
    }

    if (!coverDrawn) {
      renderer.drawIcon(LucideBookOpen32, pageWidth / 2 - 12, HOME_COVER_TOP + coverSlotHeight / 2 - 12, 24, 24);
    }

    const int titleTop = HOME_COVER_TOP + coverSlotHeight + HOME_COVER_TO_TITLE_GAP;
    for (size_t line = 0; line < titleLines.size(); ++line) {
      renderer.drawCenteredText(UI_14_FONT_ID, titleTop + static_cast<int>(line) * titleLineHeight,
                                titleLines[line].c_str(), true, EpdFontFamily::BOLD);
    }
    int detailCursorY = titleTop + titleBlockHeight;
    if (hasAuthor) {
      detailCursorY += HOME_TITLE_TO_AUTHOR_GAP;
      const char* authorLabel = hasRecentBook ? recentBook->author.c_str() : tr(STR_OPEN_LIBRARY_HINT);
      const std::string author = renderer.truncatedText(UI_10_FONT_ID, authorLabel, textWidth);
      renderer.drawCenteredText(UI_10_FONT_ID, detailCursorY, author.c_str());
      detailCursorY += metadataLineHeight;
    }
    detailCursorY += HOME_METADATA_GAP;

    char readingTimeText[40];
    const uint32_t totalMinutes = readingSummary.readingSeconds / 60;
    const uint32_t hours = totalMinutes / 60;
    const uint32_t minutes = totalMinutes % 60;
    if (hours > 0) {
      snprintf(readingTimeText, sizeof(readingTimeText), tr(STR_HOME_HOURS_MINUTES_FORMAT),
               static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
    } else {
      snprintf(readingTimeText, sizeof(readingTimeText), tr(STR_HOME_MINUTES_FORMAT),
               static_cast<unsigned long>(totalMinutes));
    }

    // With no book yet, the progress figure, the rule and the reading-time line
    // have nothing to report. Drawing "-", "-" and an empty 0% rule made the
    // first screen a new device shows look broken; leave the bands empty instead
    // so the vertical rhythm — and the action button's position — is unchanged.
    char progressSummary[64] = "";
    if (!recentBooks.empty() && readingSummary.totalPages > 0) {
      snprintf(progressSummary, sizeof(progressSummary), "%u%%  ·  %lu / %lu", readingSummary.progressPercent,
               static_cast<unsigned long>(readingSummary.currentPage),
               static_cast<unsigned long>(readingSummary.totalPages));
    } else if (!recentBooks.empty() && readingSummary.currentPage > 0) {
      snprintf(progressSummary, sizeof(progressSummary), "%u%%  ·  %lu", readingSummary.progressPercent,
               static_cast<unsigned long>(readingSummary.currentPage));
    } else if (!recentBooks.empty()) {
      snprintf(progressSummary, sizeof(progressSummary), "%u%%", readingSummary.progressPercent);
    }
    const std::string progressLine =
        renderer.truncatedText(UI_10_FONT_ID, progressSummary, pageWidth - HOME_CONTENT_MARGIN * 2);
    const int progressTextTop = detailCursorY;
    renderer.drawCenteredText(UI_10_FONT_ID, progressTextTop, progressLine.c_str());

    // Inset the rule to the same content margin as the title, author and action
    // row above and below it, instead of a hardcoded 52 px that also assumed a
    // 480 px panel.
    const int progressX = HOME_ACTION_SIDE_MARGIN;
    const int progressWidth = pageWidth - HOME_ACTION_SIDE_MARGIN * 2;
    const int progressBarTop = progressTextTop + metadataLineHeight + HOME_PROGRESS_BAR_GAP;
    if (hasRecentBook) {
      for (int x = progressX; x <= progressX + progressWidth; x += 2) {
        renderer.drawPixel(x, progressBarTop, true);
      }
      const int progressFillWidth =
          std::clamp(progressWidth * static_cast<int>(readingSummary.progressPercent) / 100, 0, progressWidth);
      if (progressFillWidth > 0) {
        renderer.drawLine(progressX, progressBarTop, progressX + progressFillWidth, progressBarTop,
                          HOME_PROGRESS_BAR_THICKNESS, true);
      }
    }

    char readingLine[96] = "";
    if (!recentBooks.empty()) {
      snprintf(readingLine, sizeof(readingLine), "%s: %s", tr(STR_READING_TIME), readingTimeText);
    }
    const std::string readingText =
        renderer.truncatedText(UI_10_FONT_ID, readingLine, pageWidth - HOME_CONTENT_MARGIN * 2);
    const int readingTimeTop = progressBarTop + HOME_PROGRESS_BAR_THICKNESS + HOME_PROGRESS_TO_TIME_GAP;
    renderer.drawCenteredText(UI_10_FONT_ID, readingTimeTop, readingText.c_str());

    const char* continueLabel = recentBooks.empty() ? tr(STR_START_READING) : tr(STR_CONTINUE_READING);
    const int continueTop = readingTimeTop + metadataLineHeight + HOME_TIME_TO_ACTION_GAP;
    const Rect continueRect{HOME_ACTION_SIDE_MARGIN, continueTop, pageWidth - HOME_ACTION_SIDE_MARGIN * 2,
                            HOME_CONTINUE_HEIGHT};
    drawHomeActionRow(renderer, continueRect, continueLabel);
  } else {
    const char* header = pageIndex == 1 ? tr(STR_LIBRARY) : tr(STR_SETTINGS_TITLE);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);

    constexpr std::array<UIIcon, 4> libraryIcons = {UIIcon::Book, UIIcon::Folder, UIIcon::Image, UIIcon::Favorite};
    const std::array<const char*, 4> libraryLabels = {tr(STR_BOOKS), tr(STR_FILES), tr(STR_GALLERY), tr(STR_FAVORITES)};
    constexpr std::array<UIIcon, 3> transferIcons = {UIIcon::Wifi, UIIcon::Library, UIIcon::Hotspot};
    const std::array<const char*, 3> transferLabels = {tr(STR_JOIN_NETWORK), tr(STR_CALIBRE_WIRELESS),
                                                       tr(STR_CREATE_HOTSPOT)};
    constexpr std::array<UIIcon, SettingsActivity::CATEGORY_COUNT> settingsIcons = {
        UIIcon::Interface, UIIcon::Power,   UIIcon::Reading, UIIcon::Controls,
        UIIcon::Files,     UIIcon::NetworkSync, UIIcon::System,
    };
    const std::array<const char*, SettingsActivity::CATEGORY_COUNT> settingsLabels = {
        tr(STR_SETTINGS_INTERFACE), tr(STR_SETTINGS_POWER),   tr(STR_SETTINGS_READING),
        tr(STR_SETTINGS_CONTROLS),  tr(STR_SETTINGS_LIBRARY), tr(STR_SETTINGS_NETWORK),
        tr(STR_SETTINGS_SYSTEM),
    };
    const int contentTop = metrics.topPadding + metrics.headerHeight + 12;
    if (pageIndex == 1) {
      GUI.drawButtonMenu(
          renderer, Rect{0, contentTop, pageWidth, pageHeight - contentTop - 96}, 4,
          selectedIndex < 4 ? selectedIndex : -1, [&](const int index) { return std::string(libraryLabels[index]); },
          [&](const int index) { return libraryIcons[index]; });

      const int transferHeaderTop = contentTop + 4 * (metrics.menuRowHeight + metrics.menuSpacing);
      GUI.drawHeader(renderer, Rect{0, transferHeaderTop, pageWidth, metrics.headerHeight},
                     tr(STR_TRANSFER_SECTION));
      const int transferTop = transferHeaderTop + metrics.headerHeight + metrics.verticalSpacing;
      // Titles only. The one-line descriptions never fit a Cyrillic or other
      // wide-script locale in the space a subtitle row leaves after the icon and
      // the chevron -- all three were cut mid-word -- and a sentence that stops
      // before its point informs nobody. Dropping them also gives this section the
      // same row rhythm as the Library tiles above it, instead of 58 px menu rows
      // followed by 76 px subtitle rows on one screen.
      GUI.drawButtonMenu(
          renderer, Rect{0, transferTop, pageWidth, pageHeight - transferTop - 96}, 3,
          selectedIndex >= 4 ? selectedIndex - 4 : -1,
          [&](const int index) { return std::string(transferLabels[index]); },
          [&](const int index) { return transferIcons[index]; });
    } else {
      GUI.drawButtonMenu(
          renderer, Rect{0, contentTop, pageWidth, pageHeight - contentTop - 96},
          SettingsActivity::CATEGORY_COUNT, selectedIndex,
          [&](const int index) { return std::string(settingsLabels[index]); },
          [&](const int index) { return settingsIcons[index]; });
    }
  }

  GUI.drawPageDots(renderer, pageIndex, PAGE_COUNT);
  const auto labels = mappedInput.mapLabels("", tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

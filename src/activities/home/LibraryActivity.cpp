#include "LibraryActivity.h"

#include <Arduino.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "FavoriteBooksStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/ProgressFile.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
bool isSupportedBook(const std::string_view filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasFb2Extension(filename) ||
         FsHelpers::hasPdfExtension(filename);
}

std::string displayTitleFromPath(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
  const auto dot = path.find_last_of('.');
  const size_t nameEnd = dot == std::string::npos || dot < nameStart ? path.size() : dot;
  return path.substr(nameStart, nameEnd - nameStart);
}

std::string displayFormatFromPath(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= path.size()) return "";
  std::string format = path.substr(dot + 1);
  std::transform(format.begin(), format.end(), format.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return format;
}

const RecentBook* findMetadata(const std::vector<RecentBook>& books, const std::string& path, int* index = nullptr) {
  for (size_t i = 0; i < books.size(); i++) {
    if (books[i].path == path) {
      if (index) *index = static_cast<int>(i);
      return &books[i];
    }
  }
  return nullptr;
}

struct LibraryProgress {
  bool opened = false;
  uint8_t percent = 0;
};

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

LibraryProgress loadLibraryProgress(const std::string& path, const bool listedInRecents) {
  LibraryProgress result{listedInRecents, 0};
  const std::string cachePath = getBookCachePath(path);
  if (cachePath.empty()) return result;

  const std::string progressPath = cachePath + "/progress.bin";
  if (!Storage.exists(progressPath.c_str())) return result;
  HalFile progressFile;
  if (!Storage.openFileForRead("LIB", progressPath, progressFile)) return result;

  result.opened = true;
  uint8_t data[7]{};
  const int bytesRead = progressFile.read(data, sizeof(data));
  progressFile.close();
  if (bytesRead == 7 && data[6] <= 100) {
    result.percent = data[6];
    return result;
  }
  if (bytesRead != 4 && bytesRead != 6) return result;

  // One-time migration for progress written by older firmware. Loading an
  // existing book.bin is cheap and never starts indexing (buildIfMissing=false).
  // Once calculated, the seventh byte makes every later library visit O(1).
  Epub epub(path, "/.crosspoint");
  if (!epub.load(false, true)) return result;

  const uint16_t spineIndex = readLe16(data);
  if (spineIndex >= epub.getSpineItemsCount()) return result;
  uint16_t pageNumber = readLe16(data + 2);
  if (pageNumber == UINT16_MAX) pageNumber = 0;
  const uint16_t pageCount = bytesRead == 6 ? readLe16(data + 4) : 0;
  const float chapterProgress = pageCount > 0 ? std::min(1.0f, static_cast<float>(pageNumber) / pageCount) : 0.0f;
  const float bookProgress = std::clamp(epub.calculateProgress(spineIndex, chapterProgress), 0.0f, 1.0f);
  result.percent = static_cast<uint8_t>(bookProgress * 100.0f + 0.5f);

  data[6] = result.percent;
  if (!ProgressFile::writeAtomic(cachePath, data, sizeof(data))) {
    LOG_ERR("LIB", "Could not upgrade progress summary for %s", path.c_str());
  }
  return result;
}

std::string makeBookSubtitle(const std::string& author, const uint8_t percent) {
  if (percent == 0) return author;
  if (author.empty()) return std::to_string(percent) + "%";
  return author + " · " + std::to_string(percent) + "%";
}
}  // namespace

void LibraryActivity::onEnter() {
  Activity::onEnter();
  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("LIB", "OOM: filename buffer");
    requestUpdate();
    return;
  }
  loading = true;
  requestUpdateAndWait();
  FAVORITE_BOOKS.pruneMissing();
  loadBooks();
  loading = false;
  selectedIndex = 0;
  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();
  books.clear();
  fileNameBuffer.reset();
}

void LibraryActivity::loadBooks() {
  books.clear();
  books.reserve(mode == Mode::Favorites ? FAVORITE_BOOKS.getBooks().size() : 48);
  if (mode == Mode::Favorites) {
    loadFavorites();
  } else {
    scanAllBooks();
  }
  sortBooks();
}

void LibraryActivity::loadFavorites() {
  const auto& favorites = FAVORITE_BOOKS.getBooks();
  const auto& recents = RECENT_BOOKS.getBooks();
  for (const RecentBook& favorite : favorites) {
    int recentRank = 1000;
    const bool isRecent = findMetadata(recents, favorite.path, &recentRank) != nullptr;
    const LibraryProgress progress = loadLibraryProgress(favorite.path, isRecent);
    books.push_back({favorite.path, favorite.title.empty() ? displayTitleFromPath(favorite.path) : favorite.title,
                     favorite.author, makeBookSubtitle(favorite.author, progress.percent),
                     displayFormatFromPath(favorite.path), recentRank, true, !progress.opened});
  }
}

void LibraryActivity::scanAllBooks() {
  if (!fileNameBuffer) return;

  std::vector<std::string> directories;
  directories.reserve(64);
  directories.emplace_back("/");
  constexpr size_t MAX_SCANNED_DIRECTORIES = 128;
  constexpr size_t MAX_SCANNED_ENTRIES = 4096;
  size_t scannedDirectories = 0;
  size_t scannedEntries = 0;

  const auto& recents = RECENT_BOOKS.getBooks();
  const auto& favorites = FAVORITE_BOOKS.getBooks();

  while (!directories.empty() && books.size() < MAX_LIBRARY_BOOKS && scannedDirectories < MAX_SCANNED_DIRECTORIES &&
         scannedEntries < MAX_SCANNED_ENTRIES) {
    std::string directory = std::move(directories.back());
    directories.pop_back();
    ++scannedDirectories;

    auto root = Storage.open(directory.c_str());
    if (!root || !root.isDirectory()) continue;
    root.rewindDirectory();

    for (auto entry = root.openNextFile(); entry && books.size() < MAX_LIBRARY_BOOKS; entry = root.openNextFile()) {
      if (++scannedEntries > MAX_SCANNED_ENTRIES) break;
      if ((scannedEntries & 0x0F) == 0) {
        // Slow or fragmented SD cards can make a bounded 4096-entry scan take
        // longer than the main-loop watchdog window.
        esp_task_wdt_reset();
        yield();
      }
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      const char* name = fileNameBuffer.get();
      if (name[0] == '\0' || strcmp(name, "System Volume Information") == 0) continue;
      if (name[0] == '.' && (!SETTINGS.showHiddenFiles || strcmp(name, ".crosspoint") == 0)) continue;

      std::string fullPath = directory;
      if (fullPath.back() != '/') fullPath += '/';
      fullPath += name;

      if (entry.isDirectory()) {
        if (directories.size() < MAX_SCANNED_DIRECTORIES) directories.push_back(std::move(fullPath));
        continue;
      }
      if (!isSupportedBook(name)) continue;

      int recentRank = 1000;
      const RecentBook* recent = findMetadata(recents, fullPath, &recentRank);
      const RecentBook* favorite = findMetadata(favorites, fullPath);
      const char* title = recent && !recent->title.empty()       ? recent->title.c_str()
                          : favorite && !favorite->title.empty() ? favorite->title.c_str()
                                                                 : nullptr;
      const char* author = recent && !recent->author.empty()       ? recent->author.c_str()
                           : favorite && !favorite->author.empty() ? favorite->author.c_str()
                                                                   : "";
      const LibraryProgress progress = loadLibraryProgress(fullPath, recent != nullptr);
      books.push_back({fullPath, title ? std::string(title) : displayTitleFromPath(fullPath), author,
                       makeBookSubtitle(author, progress.percent), displayFormatFromPath(fullPath), recentRank,
                       favorite != nullptr, !progress.opened});
    }
  }
}

const char* LibraryActivity::sortModeLabel() const {
  switch (sortMode) {
    case SortMode::Title:
      return tr(STR_SORT_TITLE);
    case SortMode::Author:
      return tr(STR_SORT_AUTHOR);
    case SortMode::Format:
      return tr(STR_SORT_FORMAT);
    case SortMode::Recent:
      return tr(STR_SORT_RECENT);
    case SortMode::Count:
      break;
  }
  return tr(STR_SORT_TITLE);
}

void LibraryActivity::sortBooks(const int direction) {
  if (direction != 0) {
    const int count = static_cast<int>(SortMode::Count);
    int value = static_cast<int>(sortMode);
    value = (value + direction + count) % count;
    sortMode = static_cast<SortMode>(value);
  }

  const auto compareText = [](const std::string& left, const std::string& right) { return left < right; };
  std::stable_sort(books.begin(), books.end(), [&](const BookEntry& left, const BookEntry& right) {
    switch (sortMode) {
      case SortMode::Author:
        if (left.author.empty() != right.author.empty()) return !left.author.empty();
        if (left.author != right.author) return compareText(left.author, right.author);
        break;
      case SortMode::Format:
        if (left.format != right.format) return compareText(left.format, right.format);
        break;
      case SortMode::Recent:
        if (left.recentRank != right.recentRank) return left.recentRank < right.recentRank;
        break;
      case SortMode::Title:
      case SortMode::Count:
        break;
    }
    return compareText(left.title, right.title);
  });
  selectedIndex = std::min(selectedIndex, books.empty() ? size_t{0} : books.size() - 1);
}

void LibraryActivity::toggleSelectedFavorite() {
  if (selectedIndex >= books.size()) return;
  BookEntry& book = books[selectedIndex];
  if (!FAVORITE_BOOKS.toggle(book.path, book.title, book.author)) {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
    renderer.displayBuffer();
    return;
  }

  if (mode == Mode::Favorites) {
    books.erase(books.begin() + selectedIndex);
    selectedIndex = std::min(selectedIndex, books.empty() ? size_t{0} : books.size() - 1);
  } else {
    book.favorite = !book.favorite;
  }
  requestUpdate(true);
}

void LibraryActivity::loop() {
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressFired = false;
    return;
  }

  if (!books.empty() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= FAVORITE_HOLD_MS) {
    longPressFired = true;
    toggleSelectedFavorite();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex < books.size()) onSelectBook(books[selectedIndex].path);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(mode == Mode::Favorites ? HomeMenuItem::FAVORITES : HomeMenuItem::LIBRARY);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down) && !books.empty()) {
    selectedIndex = (selectedIndex + 1) % books.size();
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up) && !books.empty()) {
    selectedIndex = (selectedIndex + books.size() - 1) % books.size();
    requestUpdate();
  }

  if (mode == Mode::AllBooks && mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    sortBooks(1);
    requestUpdate();
  } else if (mode == Mode::AllBooks && mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    sortBooks(-1);
    requestUpdate();
  }
}

void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const char* title = mode == Mode::Favorites ? tr(STR_FAVORITES) : tr(STR_BOOKS);

  std::string sortValue;
  // The mode name alone. At the larger type scale "Sorting: Recently opened" plus
  // the screen title plus the battery does not fit 480 px, and the prefix is the
  // half the user already knows -- it was the value that got truncated away.
  if (mode == Mode::AllBooks) sortValue = sortModeLabel();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title,
                 sortValue.empty() ? nullptr : sortValue.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = std::max(0, UITheme::getListContentBottom(renderer, !books.empty()) - contentTop);
  if (books.empty()) {
    GUI.drawEmptyState(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        loading ? tr(STR_SCANNING) : (mode == Mode::Favorites ? tr(STR_NO_FAVORITES) : tr(STR_NO_FILES_FOUND)), nullptr,
        /*script=*/true);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(books.size()),
        static_cast<int>(selectedIndex), [this](int index) { return books[index].title; },
        [this](int index) { return books[index].subtitle; },
        [this](int index) { return books[index].isNew ? UIIcon::BookNew : UITheme::getFileIcon(books[index].path); },
        // No format column. It cost roughly 70 px of every row to repeat what the
        // leading icon already says -- this is a book -- while the title, which is
        // how anyone actually picks what to read, was truncated to make room. The
        // format is still on the row in Files and on the Properties screen, where
        // it is the point rather than decoration.
        nullptr, false, nullptr,
        // The favourite marker is a real accessory. It used to be a U+2605 star
        // appended to the format string, but the UI font has no glyph for it, so
        // nothing was drawn at all and the two padding spaces left favourited rows
        // with a ragged right edge.
        [this](int index) { return books[index].favorite ? UIAccessory::Favorite : UIAccessory::None; });
    GUI.drawFooterCounter(renderer, static_cast<int>(selectedIndex), static_cast<int>(books.size()));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), books.empty() ? "" : tr(STR_OPEN),
                                            mode == Mode::AllBooks ? "<" : "", mode == Mode::AllBooks ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

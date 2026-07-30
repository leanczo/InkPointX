#include "LibraryActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "FavoriteBooksStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

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
}  // namespace

void LibraryActivity::onEnter() {
  Activity::onEnter();
  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("LIB", "OOM: filename buffer");
    requestUpdate();
    return;
  }
  FAVORITE_BOOKS.pruneMissing();
  loadBooks();
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
    findMetadata(recents, favorite.path, &recentRank);
    books.push_back({favorite.path, favorite.title.empty() ? displayTitleFromPath(favorite.path) : favorite.title,
                     favorite.author, displayFormatFromPath(favorite.path), recentRank, true});
  }
}

void LibraryActivity::scanAllBooks() {
  if (!fileNameBuffer) return;

  std::vector<std::string> directories;
  directories.reserve(16);
  directories.emplace_back("/");

  const auto& recents = RECENT_BOOKS.getBooks();
  const auto& favorites = FAVORITE_BOOKS.getBooks();

  while (!directories.empty() && books.size() < MAX_LIBRARY_BOOKS) {
    std::string directory = std::move(directories.back());
    directories.pop_back();

    auto root = Storage.open(directory.c_str());
    if (!root || !root.isDirectory()) continue;
    root.rewindDirectory();

    for (auto entry = root.openNextFile(); entry && books.size() < MAX_LIBRARY_BOOKS; entry = root.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      const char* name = fileNameBuffer.get();
      if (name[0] == '\0' || strcmp(name, "System Volume Information") == 0) continue;
      if (name[0] == '.' && (!SETTINGS.showHiddenFiles || strcmp(name, ".crosspoint") == 0)) continue;

      std::string fullPath = directory;
      if (fullPath.back() != '/') fullPath += '/';
      fullPath += name;

      if (entry.isDirectory()) {
        directories.push_back(std::move(fullPath));
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
      books.push_back({fullPath, title ? std::string(title) : displayTitleFromPath(fullPath), author,
                       displayFormatFromPath(fullPath), recentRank, favorite != nullptr});
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
  const int pageHeight = renderer.getScreenHeight();
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
    GUI.drawEmptyState(renderer, Rect{0, contentTop, pageWidth, contentHeight},
                       mode == Mode::Favorites ? tr(STR_NO_FAVORITES) : tr(STR_NO_FILES_FOUND));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(books.size()),
        static_cast<int>(selectedIndex), [this](int index) { return books[index].title; },
        [this](int index) { return books[index].author; },
        [this](int index) { return UITheme::getFileIcon(books[index].path); },
        [this](int index) { return books[index].format; }, false, nullptr,
        // The favourite marker is a real 16 px accessory. It used to be a U+2605
        // star appended to the format string, but FiraGO has no glyph for it, so
        // nothing was drawn at all and the two padding spaces left favourited
        // rows with a ragged right edge.
        [this](int index) { return books[index].favorite ? UIAccessory::Favorite : UIAccessory::None; });
    GUI.drawFooterCounter(renderer, static_cast<int>(selectedIndex), static_cast<int>(books.size()));
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_HOME), books.empty() ? "" : tr(STR_OPEN),
                            mode == Mode::AllBooks ? "<" : "", mode == Mode::AllBooks ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

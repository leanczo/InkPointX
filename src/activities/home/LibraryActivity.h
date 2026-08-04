#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/HoldGestures.h"

class LibraryActivity final : public Activity {
 public:
  enum class Mode { AllBooks, Favorites };

  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode = Mode::AllBooks)
      : Activity(mode == Mode::AllBooks ? "Library" : "Favorites", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct BookEntry {
    std::string path;
    std::string title;
    std::string author;
    std::string subtitle;
    std::string format;
    int recentRank = 1000;
    bool favorite = false;
    bool isNew = true;
  };

  enum class SortMode : uint8_t { Title, Author, Format, Recent, Count };

  static constexpr size_t MAX_LIBRARY_BOOKS = 256;
  static constexpr size_t NAME_BUFFER_SIZE = 384;
  // Acting on the selected book, so the shared short hold.
  static constexpr unsigned long FAVORITE_HOLD_MS = HoldGestures::SHORT_MS;

  Mode mode;
  std::vector<BookEntry> books;
  std::unique_ptr<char[]> fileNameBuffer;
  size_t selectedIndex = 0;
  SortMode sortMode = SortMode::Title;
  bool longPressFired = false;
  bool loading = false;

  void loadBooks();
  void scanAllBooks();
  void loadFavorites();
  void sortBooks(int direction = 0);
  void toggleSelectedFavorite();
  const char* sortModeLabel() const;
};

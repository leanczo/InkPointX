#pragma once

#include <string>
#include <vector>

#include "RecentBooksStore.h"

class FavoriteBooksStore {
  static FavoriteBooksStore instance;

  std::vector<RecentBook> favoriteBooks;

 public:
  static FavoriteBooksStore& getInstance() { return instance; }

  bool contains(const std::string& path) const;
  bool toggle(const std::string& path, const std::string& title, const std::string& author,
              const std::string& coverBmpPath = "");
  bool removeByPath(const std::string& path);
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);
  bool pruneMissing();

  const std::vector<RecentBook>& getBooks() const { return favoriteBooks; }
  bool saveToFile() const;
  bool loadFromFile();
};

#define FAVORITE_BOOKS FavoriteBooksStore::getInstance()

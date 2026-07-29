#include "FavoriteBooksStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

namespace {
constexpr char FAVORITES_FILE_JSON[] = "/.crosspoint/favorites.json";
constexpr size_t MAX_FAVORITE_BOOKS = 64;
}  // namespace

FavoriteBooksStore FavoriteBooksStore::instance;

bool FavoriteBooksStore::contains(const std::string& path) const {
  return std::any_of(favoriteBooks.begin(), favoriteBooks.end(),
                     [&path](const RecentBook& book) { return book.path == path; });
}

bool FavoriteBooksStore::toggle(const std::string& path, const std::string& title, const std::string& author,
                                const std::string& coverBmpPath) {
  const auto existing = std::find_if(favoriteBooks.begin(), favoriteBooks.end(),
                                     [&path](const RecentBook& book) { return book.path == path; });
  if (existing != favoriteBooks.end()) {
    const RecentBook removed = *existing;
    const auto removedIndex = static_cast<size_t>(std::distance(favoriteBooks.begin(), existing));
    favoriteBooks.erase(existing);
    if (saveToFile()) return true;
    favoriteBooks.insert(favoriteBooks.begin() + removedIndex, removed);
    return false;
  }

  if (favoriteBooks.size() >= MAX_FAVORITE_BOOKS) {
    LOG_ERR("FAV", "Favorites limit reached");
    return false;
  }
  favoriteBooks.push_back({path, title, author, coverBmpPath});
  if (saveToFile()) return true;
  favoriteBooks.pop_back();
  return false;
}

bool FavoriteBooksStore::removeByPath(const std::string& path) {
  const auto existing = std::find_if(favoriteBooks.begin(), favoriteBooks.end(),
                                     [&path](const RecentBook& book) { return book.path == path; });
  if (existing == favoriteBooks.end()) return false;
  const RecentBook removed = *existing;
  const auto removedIndex = static_cast<size_t>(std::distance(favoriteBooks.begin(), existing));
  favoriteBooks.erase(existing);
  if (saveToFile()) return true;
  favoriteBooks.insert(favoriteBooks.begin() + removedIndex, removed);
  return false;
}

void FavoriteBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                    const std::string& oldCachePath, const std::string& newCachePath) {
  const auto existing = std::find_if(favoriteBooks.begin(), favoriteBooks.end(),
                                     [&oldPath](const RecentBook& book) { return book.path == oldPath; });
  if (existing == favoriteBooks.end()) return;
  existing->path = newPath;
  if (!oldCachePath.empty() && !existing->coverBmpPath.empty() && existing->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    existing->coverBmpPath = newCachePath + existing->coverBmpPath.substr(oldCachePath.size());
  }
  saveToFile();
}

bool FavoriteBooksStore::pruneMissing() {
  const auto oldSize = favoriteBooks.size();
  favoriteBooks.erase(std::remove_if(favoriteBooks.begin(), favoriteBooks.end(),
                                     [](const RecentBook& book) { return RecentBooksStore::isMissing(book); }),
                      favoriteBooks.end());
  if (favoriteBooks.size() == oldSize) return false;
  return saveToFile();
}

bool FavoriteBooksStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  JsonArray books = doc["books"].to<JsonArray>();
  for (const RecentBook& book : favoriteBooks) {
    JsonObject entry = books.add<JsonObject>();
    entry["path"] = book.path;
    entry["title"] = book.title;
    entry["author"] = book.author;
    entry["cover"] = book.coverBmpPath;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(FAVORITES_FILE_JSON, json);
}

bool FavoriteBooksStore::loadFromFile() {
  favoriteBooks.clear();
  favoriteBooks.reserve(16);
  if (!Storage.exists(FAVORITES_FILE_JSON)) return true;

  const String json = Storage.readFile(FAVORITES_FILE_JSON);
  if (json.isEmpty()) return false;

  JsonDocument doc;
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("FAV", "JSON parse error: %s", error.c_str());
    return false;
  }

  JsonArrayConst books = doc["books"];
  for (JsonObjectConst entry : books) {
    if (favoriteBooks.size() >= MAX_FAVORITE_BOOKS) break;
    const char* path = entry["path"] | "";
    if (path[0] == '\0') continue;
    favoriteBooks.push_back({path, entry["title"] | "", entry["author"] | "", entry["cover"] | ""});
  }
  pruneMissing();
  return true;
}

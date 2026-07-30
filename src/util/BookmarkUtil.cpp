#include "BookmarkUtil.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <string>

std::string BookmarkUtil::getBookmarksDir() { return "/.crosspoint/bookmarks/"; }

namespace {
// The pre-migration scheme, kept only so existing bookmark files can be found and
// renamed once. It dropped the extension, so /books/a.epub, /books/a.txt and
// /books_a.epub all collapsed onto the same file.
std::string legacyBookmarkFileName(const std::string& bookPath) {
  std::string bookName = std::string(bookPath).erase(0, 1);
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');
  const size_t lastDot = bookName.find_last_of('.');
  if (lastDot != std::string::npos) {
    bookName.erase(lastDot);
  }
  return bookName + ".json";
}
}  // namespace

std::string BookmarkUtil::getBookmarkPath(const std::string& bookPath) {
  // remove leading slash and replace internal slashes to create a flat filename
  std::string bookName = std::string(bookPath).erase(0, 1);
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');
  // Keep the extension in the key so two books cannot share one bookmark file.
  std::replace(bookName.begin(), bookName.end(), '.', '_');
  const std::string path = getBookmarksDir() + bookName + ".json";

  // One-shot migration: adopt the file written under the old scheme so bookmarks
  // saved by an earlier firmware are not silently orphaned. Only when there is no
  // file at the new path, so a collided legacy file can never overwrite real data.
  if (!Storage.exists(path.c_str())) {
    const std::string legacyPath = getBookmarksDir() + legacyBookmarkFileName(bookPath);
    if (legacyPath != path && Storage.exists(legacyPath.c_str())) {
      if (Storage.rename(legacyPath.c_str(), path.c_str())) {
        LOG_DBG("BMK", "Migrated bookmarks: %s -> %s", legacyPath.c_str(), path.c_str());
      } else {
        LOG_ERR("BMK", "Could not migrate bookmarks from %s", legacyPath.c_str());
      }
    }
  }

  return path;
}

std::string BookmarkUtil::sanitizeBookmarkSummary(std::string summary) {
  summary.erase(
      std::unique(summary.begin(), summary.end(), [](char a, char b) { return std::isspace(a) && std::isspace(b); }),
      summary.end());
  summary.erase(std::remove(summary.begin(), summary.end(), '\n'), summary.end());
  summary.erase(summary.begin(),
                std::find_if(summary.begin(), summary.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  summary.erase(
      std::find_if(summary.rbegin(), summary.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
      summary.end());
  if (summary.size() > 72) {
    summary.resize(72);
  }
  return summary;
}

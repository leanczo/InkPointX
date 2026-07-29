#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, TXT, Markdown, or FB2). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Returns the cache directory keyed by a supported book path, or an empty string for non-books.
std::string getBookCachePath(const std::string& path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);

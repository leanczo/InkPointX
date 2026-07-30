#include "Txt.h"

#include <Fb2Encoding.h>
#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>

#include <algorithm>

namespace {
// Enough text to judge the high-byte distribution without a large stack buffer.
constexpr size_t ENCODING_SAMPLE_BYTES = 1024;
}  // namespace

Txt::Txt(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), contentPath(filepath), cacheBasePath(std::move(cacheBasePath)) {
  // Generate cache path from file path hash
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/txt_" + std::to_string(hash);
}

Txt::Txt(std::string sourcePath, std::string contentPath, std::string cachePath, std::string title,
         std::string author)
    : filepath(std::move(sourcePath)),
      contentPath(std::move(contentPath)),
      cachePath(std::move(cachePath)),
      title(std::move(title)),
      author(std::move(author)) {}

bool Txt::load() {
  if (loaded) {
    return true;
  }

  if (!Storage.exists(contentPath.c_str())) {
    LOG_ERR("TXT", "Content file does not exist: %s", contentPath.c_str());
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("TXT", contentPath, file)) {
    LOG_ERR("TXT", "Failed to open file: %s", contentPath.c_str());
    return false;
  }

  fileSize = file.size();

  // Sample the head of the file to guess the encoding. Plain text carries no
  // declaration, and the reader previously fed raw bytes straight to the glyph
  // renderer — so a CP1251 or KOI8-R book displayed as a wall of replacement
  // characters, and a UTF-8 BOM showed as a stray glyph on page one.
  char sample[ENCODING_SAMPLE_BYTES];
  const size_t sampleLength = file.read(sample, std::min(sizeof(sample), fileSize));
  encoding = Fb2Encoding::detect(sample, sampleLength);
  contentStart = Fb2Encoding::bomLength(sample, sampleLength);
  file.close();

  loaded = true;
  LOG_DBG("TXT", "Loaded TXT file: %s (%zu bytes, encoding %s%s)", filepath.c_str(), fileSize, encoding,
          contentStart > 0 ? ", BOM" : "");
  return true;
}

const char* Txt::getEncoding() const { return encoding ? encoding : Fb2Encoding::UTF8; }

std::string Txt::getTitle() const {
  if (!title.empty()) {
    return title;
  }

  // Extract filename without path and extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove the plain-text or Markdown extension.
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    const size_t dot = filename.find_last_of('.');
    filename = filename.substr(0, dot);
  }

  return filename;
}

void Txt::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

std::string Txt::findCoverImage() const {
  // Get the folder containing the txt file
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) {
    folder = "/";
  }

  // Get the base filename without extension (e.g., "mybook" from "/books/mybook.txt")
  std::string baseName = getTitle();

  // Image extensions to try
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  // First priority: look for image with same name as txt file (e.g., mybook.jpg)
  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) {
      LOG_DBG("TXT", "Found matching cover image: %s", coverPath.c_str());
      return coverPath;
    }
  }

  // Fallback: look for cover image files
  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) {
        LOG_DBG("TXT", "Found fallback cover image: %s", coverPath.c_str());
        return coverPath;
      }
    }
  }

  return "";
}

std::string Txt::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Txt::generateCoverBmp() const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    LOG_DBG("TXT", "No cover image found for TXT file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  if (FsHelpers::hasBmpExtension(coverImagePath)) {
    // Copy BMP file to cache
    LOG_DBG("TXT", "Copying BMP cover image to cache");
    HalFile src, dst;
    if (!Storage.openFileForRead("TXT", coverImagePath, src)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", getCoverBmpPath(), dst)) {
      return false;
    }
    uint8_t buffer[1024];
    while (src.available()) {
      size_t bytesRead = src.read(buffer, sizeof(buffer));
      dst.write(buffer, bytesRead);
    }
    LOG_DBG("TXT", "Copied BMP cover to cache");
    return true;
  } else if (FsHelpers::hasJpgExtension(coverImagePath)) {
    // Convert JPG/JPEG to BMP (same approach as Epub)
    LOG_DBG("TXT", "Generating BMP from JPG cover image");
    HalFile coverJpg, coverBmp;
    if (!Storage.openFileForRead("TXT", coverImagePath, coverJpg)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", getCoverBmpPath(), coverBmp)) {
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);

    if (!success) {
      LOG_ERR("TXT", "Failed to generate BMP from JPG cover image");
      Storage.remove(getCoverBmpPath().c_str());
    } else {
      LOG_DBG("TXT", "Generated BMP from JPG cover image");
    }
    return success;
  }

  // PNG files are not supported (would need a PNG decoder)
  LOG_ERR("TXT", "Cover image format not supported (only BMP/JPG/JPEG)");
  return false;
}

bool Txt::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("TXT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("TXT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("TXT", "Cache cleared successfully");
  return true;
}

bool Txt::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("TXT", contentPath, file)) {
    return false;
  }

  if (!file.seek(offset)) {
    return false;
  }

  size_t bytesRead = file.read(buffer, length);
  return bytesRead > 0;
}

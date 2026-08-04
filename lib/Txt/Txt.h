#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

class Txt {
  std::string filepath;
  std::string contentPath;
  std::string cacheBasePath;
  std::string cachePath;
  std::string title;
  std::string author;
  bool loaded = false;
  size_t fileSize = 0;
  uint64_t sourceFingerprint = 0;
  // .txt carries no encoding label, so it is detected from the file's own bytes
  // when the book is loaded. Points at one of Fb2Encoding's canonical names.
  const char* encoding = nullptr;
  size_t contentStart = 0;  // bytes to skip for a byte-order mark

 public:
  explicit Txt(std::string path, std::string cacheBasePath);
  Txt(std::string sourcePath, std::string contentPath, std::string cachePath, std::string title,
      std::string author = "");

  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] const std::string& getAuthor() const { return author; }
  [[nodiscard]] size_t getFileSize() const { return fileSize; }
  [[nodiscard]] uint64_t getSourceFingerprint() const { return sourceFingerprint; }
  // Detected encoding, as one of Fb2Encoding's canonical names.
  [[nodiscard]] const char* getEncoding() const;
  // First byte of actual text: non-zero when the file starts with a BOM.
  [[nodiscard]] size_t getContentStart() const { return contentStart; }

  void setupCacheDir() const;
  bool clearCache() const;

  // Cover image support - looks for cover.bmp/jpg/jpeg/png in same folder as txt file
  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] bool generateCoverBmp() const;
  [[nodiscard]] std::string findCoverImage() const;

  // Read content from file
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;
};

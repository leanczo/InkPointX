#pragma once

#include <HalStorage.h>
#include <pdfio.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Native, streaming PDF -> unpacked EPUB-package adapter. All parsing and
// cache generation happens on the reader from the PDF stored on the SD card.
// The generated package is consumed by the existing EPUB reader so PDF gets
// the same reflow, navigation, progress, bookmarks, typography and cover flow.
class Pdf {
 public:
  using ProgressCallback = void (*)(void* context, size_t page, size_t pageCount);

 private:
  struct CMapEntry {
    uint64_t key = 0;
    uint8_t textLength = 0;
    char text[15]{};
  };

  struct FontDecoder {
    uint32_t encoding[256]{};
    std::unique_ptr<CMapEntry[]> cmap;
    size_t cmapSize = 0;
    size_t cmapLimit = 0;
    uint8_t maxCodeBytes = 1;
  };

  struct PageImage {
    std::string resourceName;
    std::string filename;
    bool fullPage = false;
  };

  struct InputContext {
    HalFile file;
    HalFile xref;
    uint64_t size = 0;
    uint64_t xrefSize = 0;
    std::string xrefPath;
  };

  std::string filepath;
  std::string cachePath;
  std::string packagePath;
  std::string title;
  std::string author;
  std::string language = "und";
  std::string lastError;
  uint64_t sourceSize = 0;
  size_t pageCount = 0;
  bool loaded = false;
  bool packageBuiltDuringLoad = false;
  pdfio_file_t* document = nullptr;
  HalFile imageRecords;
  size_t imageCount = 0;
  ProgressCallback progressCallback = nullptr;
  void* progressContext = nullptr;
  uint8_t* rasterScratch = nullptr;
  size_t rasterScratchSize = 0;

  static ssize_t inputRead(void* context, void* data, size_t length);
  static off_t inputSeek(void* context, off_t offset, int whence);
  static void inputClose(void* context);
  static bool xrefAccess(void* context, size_t number, pdfio_xref_t* record, bool writeRecord);
  static bool pdfError(pdfio_file_t* pdf, const char* message, void* context);

  bool openDocument();
  void closeDocument();
  bool convertToPackage();
  bool cacheIsCurrent();
  bool loadMetadataCache();
  void saveMetadataCache() const;
  void saveCacheSignature() const;
  void postProcessMetadata();

  static pdfio_dict_t* resolveDict(pdfio_dict_t* dict, const char* key);
  static pdfio_dict_t* findInheritedDict(pdfio_obj_t* page, const char* key);
  static pdfio_obj_t* findFontObject(pdfio_obj_t* page, const std::string& resourceName);
  static uint32_t glyphNameToUnicode(const char* name);
  bool loadFontDecoder(pdfio_obj_t* page, const std::string& resourceName, FontDecoder& decoder);
  bool loadToUnicode(pdfio_obj_t* font, FontDecoder& decoder);
  static void addCMapEntry(FontDecoder& decoder, uint32_t source, uint8_t sourceBytes, const std::string& text);
  static std::string decodeUtf16Hex(const char* token, uint32_t increment = 0);
  static bool parseHexSource(const char* token, uint32_t& value, uint8_t& bytes);
  static std::string decodeTextToken(const char* token, const FontDecoder& decoder);

  bool extractPageImages(pdfio_obj_t* page, size_t pageIndex, std::vector<PageImage>& pageImages);
  bool extractJpeg(pdfio_obj_t* imageObject, const std::string& outputPath);
  bool recordImage(const std::string& filename);
  bool writePage(pdfio_obj_t* page, size_t pageIndex, const std::vector<PageImage>& pageImages);
  bool writeContainerFile() const;
  bool writeStyleFile() const;
  bool writeOpfFile() const;
  bool writeNcxFile() const;

 public:
  explicit Pdf(std::string path, std::string cacheBasePath);
  ~Pdf();

  Pdf(const Pdf&) = delete;
  Pdf& operator=(const Pdf&) = delete;

  bool load();
  bool clearCache() const;
  void setupCacheDir() const;
  void setProgressCallback(ProgressCallback callback, void* context) {
    progressCallback = callback;
    progressContext = context;
  }
  void setRasterScratch(uint8_t* buffer, size_t size) {
    rasterScratch = buffer;
    rasterScratchSize = size;
  }

  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] const std::string& getPackagePath() const { return packagePath; }
  [[nodiscard]] const std::string& getTitle() const { return title; }
  [[nodiscard]] const std::string& getAuthor() const { return author; }
  [[nodiscard]] const std::string& getLanguage() const { return language; }
  [[nodiscard]] const std::string& getLastError() const { return lastError; }
  [[nodiscard]] uint64_t getSourceSize() const { return sourceSize; }
  [[nodiscard]] size_t getPageCount() const { return pageCount; }
  [[nodiscard]] bool isLoaded() const { return loaded; }
  [[nodiscard]] bool builtPackageDuringLoad() const { return packageBuiltDuringLoad; }
};

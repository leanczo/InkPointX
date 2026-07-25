#pragma once
#include <memory>

#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;
class Txt;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  static std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static std::unique_ptr<Epub> loadFb2AsEpub(const std::string& path);
  std::unique_ptr<Epub> loadPdfAsEpub(const std::string& path);
  static void pdfProgressCallback(void* context, size_t page, size_t pageCount);
  void renderPdfProgress(size_t page, size_t pageCount);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isFb2File(const std::string& path);
  static bool isPdfFile(const std::string& path);
  static bool isBmpFile(const std::string& path);

  int lastPdfProgressBucket = -1;

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath)
      : Activity("Reader", renderer, mappedInput), initialBookPath(std::move(initialBookPath)) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};

#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Memory.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "Fb2.h"
#include "Pdf.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isFb2File(const std::string& path) { return FsHelpers::hasFb2Extension(path); }

bool ReaderActivity::isPdfFile(const std::string& path) { return FsHelpers::hasPdfExtension(path); }

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  if (epub->load(true, SETTINGS.embeddedStyle == 0)) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
  if (!xtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return nullptr;
  }
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
  if (!txt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return nullptr;
  }
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

std::unique_ptr<Epub> ReaderActivity::loadFb2AsEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto fb2 = makeUniqueNoThrow<Fb2>(path, "/.crosspoint");
  if (!fb2 || !fb2->load()) {
    LOG_ERR("READER", "Failed to load FB2");
    return nullptr;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint", fb2->getPackagePath());
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB-compatible FB2 object");
    return nullptr;
  }
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_ERR("READER", "Failed to load EPUB-compatible FB2 package");
    return nullptr;
  }
  return epub;
}

void ReaderActivity::pdfProgressCallback(void* context, size_t page, size_t pageCount) {
  static_cast<ReaderActivity*>(context)->renderPdfProgress(page, pageCount);
}

void ReaderActivity::renderPdfProgress(size_t page, size_t pageCount) {
  if (pageCount == 0) return;
  const int bucket = static_cast<int>((page * 10) / pageCount);
  if (bucket == lastPdfProgressBucket && page < pageCount) return;
  lastPdfProgressBucket = bucket;
  char message[96];
  snprintf(message, sizeof(message), tr(STR_PDF_PREPARING_FORMAT), static_cast<unsigned>(std::min(page + 1, pageCount)),
           static_cast<unsigned>(pageCount));
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, (renderer.getScreenHeight() - lineHeight) / 2, message, true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

std::unique_ptr<Epub> ReaderActivity::loadPdfAsEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) return nullptr;
  auto pdf = makeUniqueNoThrow<Pdf>(path, "/.crosspoint");
  if (!pdf) return nullptr;
  lastPdfProgressBucket = -1;
  pdf->setProgressCallback(pdfProgressCallback, this);
  if (!pdf->load()) {
    LOG_ERR("READER", "Failed to load PDF: %s", pdf->getLastError().c_str());
    return nullptr;
  }
  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint", pdf->getPackagePath());
  if (!epub || !epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_ERR("READER", "Failed to load EPUB-compatible PDF package");
    return nullptr;
  }
  return epub;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc)));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt)));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  sdFontSystem.ensureLoaded(renderer);

  // A book that fails to load must say so: silently bouncing back to the file
  // browser read as a dead tap on the file the user just chose.
  const auto reportOpenFailure = [this] {
    GUI.drawPopup(renderer, tr(STR_BOOK_OPEN_FAILED));
    renderer.displayBuffer();
    delay(1500);
    onGoBack();
  };

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      reportOpenFailure();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      reportOpenFailure();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else if (isFb2File(initialBookPath)) {
    auto epub = loadFb2AsEpub(initialBookPath);
    if (!epub) {
      reportOpenFailure();
      return;
    }
    onGoToEpubReader(std::move(epub));
  } else if (isPdfFile(initialBookPath)) {
    auto epub = loadPdfAsEpub(initialBookPath);
    if (!epub) {
      reportOpenFailure();
      return;
    }
    onGoToEpubReader(std::move(epub));
  } else {
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      reportOpenFailure();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }

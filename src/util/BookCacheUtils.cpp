#include "BookCacheUtils.h"

#include <Epub.h>
#include <Fb2.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Pdf.h>
#include <Txt.h>
#include <Xtc.h>

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";
  constexpr char FB2_PREFIX[] = "fb2_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0 ||
         strncmp(name, FB2_PREFIX, std::size(FB2_PREFIX) - 1) == 0;
}

std::string getBookCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return Txt(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasFb2Extension(path)) {
    return Fb2(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasPdfExtension(path)) {
    return Pdf(path, "/.crosspoint").getCachePath();
  }
  return "";
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasTxtExtension(path)) {
    Txt(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasMarkdownExtension(path)) {
    Txt(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasFb2Extension(path)) {
    Fb2(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasPdfExtension(path)) {
    Pdf(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

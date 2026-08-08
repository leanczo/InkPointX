#pragma once

#include <Epub.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ProgressFile.h"

namespace EpubReaderUtils {

inline void writeLe32(uint8_t* dst, const uint32_t value) {
  dst[0] = value & 0xFF;
  dst[1] = (value >> 8) & 0xFF;
  dst[2] = (value >> 16) & 0xFF;
  dst[3] = (value >> 24) & 0xFF;
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  // Byte 6 is a denormalized whole-book percentage for lightweight library
  // rendering. Bytes 7..14 contain estimated whole-book page counters for the
  // Home screen. Existing readers intentionally read only the first six bytes,
  // so this remains backward-compatible with every progress.bin consumer.
  uint8_t data[15]{};
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const float chapterProgress = pageCount > 0 ? static_cast<float>(pageNumber) / pageCount : 0.0f;
  const float bookProgress = epub.calculateProgress(spineIndex, std::clamp(chapterProgress, 0.0f, 1.0f));
  data[6] = static_cast<uint8_t>(std::clamp(static_cast<int>(bookProgress * 100.0f + 0.5f), 0, 100));

  uint32_t currentBookPage = 0;
  uint32_t totalBookPages = 0;
  if (pageCount > 0 && spineIndex < static_cast<int>(epub.getSpineItemsCount())) {
    const size_t previousBytes = spineIndex > 0 ? epub.getCumulativeSpineItemSize(spineIndex - 1) : 0;
    const size_t cumulativeBytes = epub.getCumulativeSpineItemSize(spineIndex);
    const size_t chapterBytes = cumulativeBytes > previousBytes ? cumulativeBytes - previousBytes : 0;
    const size_t bookBytes = epub.getBookSize();
    if (chapterBytes > 0 && bookBytes > 0) {
      const double bytesPerPage = static_cast<double>(chapterBytes) / pageCount;
      const double totalEstimate = std::ceil(static_cast<double>(bookBytes) / bytesPerPage);
      const double currentEstimate =
          std::round((static_cast<double>(previousBytes) + chapterBytes * chapterProgress) / bytesPerPage);
      totalBookPages = static_cast<uint32_t>(std::min(totalEstimate, static_cast<double>(UINT32_MAX)));
      currentBookPage = static_cast<uint32_t>(std::min(currentEstimate, static_cast<double>(UINT32_MAX)));
      if (totalBookPages > 0) currentBookPage = std::clamp<uint32_t>(currentBookPage, 1, totalBookPages);
    }
  }
  writeLe32(data + 7, currentBookPage);
  writeLe32(data + 11, totalBookPages);
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, sizeof(data))) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils

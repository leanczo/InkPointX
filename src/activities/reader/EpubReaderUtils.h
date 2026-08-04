#pragma once

#include <Epub.h>
#include <Logging.h>

#include <algorithm>

#include "ProgressFile.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  // Byte 6 is a denormalized whole-book percentage for lightweight library
  // rendering. Existing readers intentionally read only the first six bytes,
  // so this remains backward-compatible with every progress.bin consumer.
  uint8_t data[7];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const float chapterProgress = pageCount > 0 ? static_cast<float>(pageNumber) / pageCount : 0.0f;
  const float bookProgress = epub.calculateProgress(spineIndex, std::clamp(chapterProgress, 0.0f, 1.0f));
  data[6] = static_cast<uint8_t>(std::clamp(static_cast<int>(bookProgress * 100.0f + 0.5f), 0, 100));
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, sizeof(data))) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils

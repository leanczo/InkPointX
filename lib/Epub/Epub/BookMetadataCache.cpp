#include "BookMetadataCache.h"

#include <CacheIntegrity.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <deque>

#include "FsHelpers.h"

namespace {
// v9 adds a bounded source fingerprint. Size alone cannot distinguish a book
// replaced in-place by another EPUB of exactly the same length.
constexpr uint8_t BOOK_CACHE_VERSION = 9;
constexpr uint16_t MAX_SPINE_ENTRIES = 8192;
constexpr uint16_t MAX_TOC_ENTRIES = 16384;
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpBookBinFile[] = "/book.bin.tmp";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  return Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile);
}

bool BookMetadataCache::endContentOpfPass() {
  // Explicit close() required: member variable persists beyond function scope
  spineFile.close();
  return true;
}

bool BookMetadataCache::beginTocPass() {
  LOG_DBG("BMC", "Beginning toc pass");

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variable persists beyond function scope
    spineFile.close();
    return false;
  }

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex.clear();
    spineHrefIndex.resize(spineCount);
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      SpineHrefIndexEntry idx;
      idx.hrefHash = fnvHash64(entry.href);
      idx.hrefLen = static_cast<uint16_t>(entry.href.size());
      idx.spineIndex = static_cast<int16_t>(i);
      spineHrefIndex[i] = idx;
    }
    std::sort(spineHrefIndex.begin(), spineHrefIndex.end(),
              [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
              });
    spineFile.seek(0);
    useSpineHrefIndex = true;
    LOG_DBG("BMC", "Using fast index for %d spine items", spineCount);
  } else {
    useSpineHrefIndex = false;
  }

  return true;
}

bool BookMetadataCache::endTocPass() {
  // Explicit close() required: member variables persist beyond function scope
  tocFile.close();
  spineFile.close();

  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;

  return true;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  LOG_DBG("BMC", "Wrote %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata,
                                     const std::string& looseItemRoot) {
  cache_integrity::SourceFingerprint sourceFingerprint;
  if (!cache_integrity::fingerprintFile(sourcePath, sourceFingerprint)) {
    LOG_ERR("BMC", "Could not fingerprint source book");
    return false;
  }
  if (spineCount == 0 || spineCount > MAX_SPINE_ENTRIES || tocCount > MAX_TOC_ENTRIES) {
    LOG_ERR("BMC", "Refusing implausible metadata counts: spine=%u toc=%u", spineCount, tocCount);
    return false;
  }
  const auto stringFits = [](const std::string& value) {
    return value.size() <= serialization::MAX_SERIALIZED_STRING_LENGTH;
  };
  if (!stringFits(metadata.title) || !stringFits(metadata.author) || !stringFits(metadata.language) ||
      !stringFits(metadata.coverItemHref) || !stringFits(metadata.textReferenceHref)) {
    LOG_ERR("BMC", "Metadata string exceeds cache format limit");
    return false;
  }
  const std::string finalBookPath = cachePath + bookBinFile;
  const std::string tempBookPath = cachePath + tmpBookBinFile;
  if (Storage.exists(tempBookPath.c_str())) Storage.remove(tempBookPath.c_str());

  // Build a complete sibling generation; keep the currently valid cache until
  // the final checked swap succeeds.
  if (!Storage.openFileForWrite("BMC", tempBookPath, bookFile)) {
    return false;
  }

  const auto failBuild = [this, &tempBookPath]() {
    bookFile.close();
    spineFile.close();
    tocFile.close();
    Storage.remove(tempBookPath.c_str());
    return false;
  };

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    // Explicit close() required: member variable persists beyond function scope
    return failBuild();
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variables persist beyond function scope
    return failBuild();
  }

  constexpr uint32_t headerASize = sizeof(BOOK_CACHE_VERSION) + sizeof(sourceFingerprint.size) +
                                   sizeof(sourceFingerprint.sampleHash) + /* LUT Offset */ sizeof(uint32_t) +
                                   sizeof(spineCount) + sizeof(tocCount);
  const uint64_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                sizeof(uint32_t) * 5ull;
  const uint64_t lutSize64 = sizeof(uint32_t) * (static_cast<uint64_t>(spineCount) + tocCount);
  if (headerASize + metadataSize + lutSize64 > UINT32_MAX) {
    return failBuild();
  }
  const uint32_t lutSize = static_cast<uint32_t>(lutSize64);
  const uint32_t lutOffset = static_cast<uint32_t>(headerASize + metadataSize);

  // Header A
  bool writeOk = serialization::writePod(bookFile, BOOK_CACHE_VERSION) &&
                 serialization::writePod(bookFile, sourceFingerprint.size) &&
                 serialization::writePod(bookFile, sourceFingerprint.sampleHash) &&
                 serialization::writePod(bookFile, lutOffset) && serialization::writePod(bookFile, spineCount) &&
                 serialization::writePod(bookFile, tocCount);
  // Metadata
  writeOk = writeOk && serialization::writeString(bookFile, metadata.title) &&
            serialization::writeString(bookFile, metadata.author) &&
            serialization::writeString(bookFile, metadata.language) &&
            serialization::writeString(bookFile, metadata.coverItemHref) &&
            serialization::writeString(bookFile, metadata.textReferenceHref);
  if (!writeOk) {
    return failBuild();
  }

  // Loop through spine entries, writing LUT positions
  spineFile.seek(0);
  for (int i = 0; i < spineCount; i++) {
    uint32_t pos = spineFile.position();
    auto spineEntry = readSpineEntry(spineFile);
    writeOk = writeOk && !spineEntry.href.empty() && serialization::writePod(bookFile, pos + lutOffset + lutSize);
  }

  // Loop through toc entries, writing LUT positions
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    uint32_t pos = tocFile.position();
    auto tocEntry = readTocEntry(tocFile);
    writeOk =
        writeOk && !tocEntry.href.empty() &&
        serialization::writePod(bookFile, pos + lutOffset + lutSize + static_cast<uint32_t>(spineFile.position()));
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m))
  std::deque<int16_t> spineToTocIndex(spineCount, -1);
  tocFile.seek(0);
  for (int j = 0; j < tocCount; j++) {
    auto tocEntry = readTocEntry(tocFile);
    if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < spineCount) {
      if (spineToTocIndex[tocEntry.spineIndex] == -1) {
        spineToTocIndex[tocEntry.spineIndex] = static_cast<int16_t>(j);
      }
    }
  }

  ZipFile zip(epubPath);
  const bool loosePackage = !looseItemRoot.empty();
  // Pre-open zip file to speed up size calculations. FB2-derived packages are
  // ordinary files in a cache directory and deliberately skip ZipFile.
  if (!loosePackage && !zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    // Explicit close() required: member variables persist beyond function scope
    return failBuild();
  }
  // NOTE: We intentionally skip calling loadAllFileStatSlims() here.
  // For large EPUBs (2000+ chapters), pre-loading all ZIP central directory entries
  // into memory causes OOM crashes on ESP32-C3's limited ~380KB RAM.
  // Instead, for large books we use a one-pass batch lookup that scans the ZIP
  // central directory once and matches against spine targets using hash comparison.
  // This is O(n*log(m)) instead of O(n*m) while avoiding memory exhaustion.
  // See: https://github.com/crosspoint-reader/crosspoint-reader/issues/134

  std::deque<uint32_t> spineSizes;
  bool useBatchSizes = false;

  if (!loosePackage && spineCount >= LARGE_SPINE_THRESHOLD) {
    LOG_DBG("BMC", "Using batch size lookup for %d spine items", spineCount);

    std::deque<ZipFile::SizeTarget> targets;
    targets.resize(spineCount);

    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      std::string path = FsHelpers::normalisePath(entry.href);

      ZipFile::SizeTarget t;
      t.hash = ZipFile::fnvHash64(path.c_str(), path.size());
      t.len = static_cast<uint16_t>(path.size());
      t.index = static_cast<uint16_t>(i);
      targets[i] = t;
    }

    std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    });

    spineSizes.resize(spineCount, 0);
    [[maybe_unused]] int matched = zip.fillUncompressedSizes(targets, spineSizes);
    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);

    targets.clear();
    targets.shrink_to_fit();

    useBatchSizes = true;
  }

  uint32_t cumSize = 0;
  spineFile.seek(0);
  int lastSpineTocIndex = -1;
  for (int i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntry(spineFile);

    spineEntry.tocIndex = spineToTocIndex[i];

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (spineEntry.tocIndex == -1) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
              spineEntry.href.c_str());
      spineEntry.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineEntry.tocIndex;

    size_t itemSize = 0;
    if (loosePackage) {
      const std::string path = FsHelpers::normalisePath(spineEntry.href);
      const std::string itemPath = looseItemRoot + "/" + path;
      HalFile itemFile;
      if (Storage.openFileForRead("BMC", itemPath, itemFile)) {
        itemSize = static_cast<size_t>(itemFile.fileSize64());
        itemFile.close();
      } else {
        LOG_ERR("BMC", "Warning: Could not get size for loose spine item: %s", path.c_str());
      }
    } else if (useBatchSizes) {
      itemSize = spineSizes[i];
      if (itemSize == 0) {
        const std::string path = FsHelpers::normalisePath(spineEntry.href);
        if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
          LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
        }
      }
    } else {
      const std::string path = FsHelpers::normalisePath(spineEntry.href);
      if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
        LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
      }
    }

    if (itemSize > UINT32_MAX - cumSize) {
      LOG_ERR("BMC", "Cumulative uncompressed EPUB size exceeds cache format");
      if (!loosePackage) zip.close();
      return failBuild();
    }
    cumSize += static_cast<uint32_t>(itemSize);
    spineEntry.cumulativeSize = cumSize;

    // Write out spine data to book.bin
    writeOk = writeOk && writeSpineEntry(bookFile, spineEntry) != UINT32_MAX;
  }
  // Close opened zip file
  if (!loosePackage) zip.close();

  // Loop through toc entries from toc file writing to book.bin
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    auto tocEntry = readTocEntry(tocFile);
    writeOk = writeOk && writeTocEntry(bookFile, tocEntry) != UINT32_MAX;
  }

  // Explicit close() required: member variables persist beyond function scope
  bookFile.flush();
  bookFile.close();
  spineFile.close();
  tocFile.close();

  if (!writeOk || !Storage.replaceFileFromTemp(finalBookPath.c_str(), tempBookPath.c_str())) {
    Storage.remove(tempBookPath.c_str());
    LOG_ERR("BMC", "Failed to publish complete book metadata cache");
    return false;
  }

  LOG_DBG("BMC", "Successfully built book.bin");
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    Storage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    Storage.remove(tocBinFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(HalFile& file, const SpineEntry& entry) const {
  const uint32_t pos = file.position();
  return serialization::writeString(file, entry.href) && serialization::writePod(file, entry.cumulativeSize) &&
                 serialization::writePod(file, entry.tocIndex)
             ? pos
             : UINT32_MAX;
}

uint32_t BookMetadataCache::writeTocEntry(HalFile& file, const TocEntry& entry) const {
  const uint32_t pos = file.position();
  return serialization::writeString(file, entry.title) && serialization::writeString(file, entry.href) &&
                 serialization::writeString(file, entry.anchor) && serialization::writePod(file, entry.level) &&
                 serialization::writePod(file, entry.spineIndex)
             ? pos
             : UINT32_MAX;
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }
  if (spineCount >= MAX_SPINE_ENTRIES || href.empty() || href.size() > serialization::MAX_SERIALIZED_STRING_LENGTH) {
    LOG_ERR("BMC", "Ignoring invalid/excess spine entry");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  if (writeSpineEntry(spineFile, entry) == UINT32_MAX) {
    LOG_ERR("BMC", "Failed to write spine entry");
    return;
  }
  ++spineCount;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }
  if (tocCount >= MAX_TOC_ENTRIES || href.empty() || title.size() > serialization::MAX_SERIALIZED_STRING_LENGTH ||
      href.size() > serialization::MAX_SERIALIZED_STRING_LENGTH ||
      anchor.size() > serialization::MAX_SERIALIZED_STRING_LENGTH) {
    LOG_ERR("BMC", "Ignoring invalid/excess TOC entry");
    return;
  }

  int16_t spineIndex = -1;

  if (useSpineHrefIndex) {
    uint64_t targetHash = fnvHash64(href);
    uint16_t targetLen = static_cast<uint16_t>(href.size());

    auto it =
        std::lower_bound(spineHrefIndex.begin(), spineHrefIndex.end(), SpineHrefIndexEntry{targetHash, targetLen, 0},
                         [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                           return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                         });

    while (it != spineHrefIndex.end() && it->hrefHash == targetHash && it->hrefLen == targetLen) {
      spineIndex = it->spineIndex;
      break;
    }

    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  } else {
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto spineEntry = readSpineEntry(spineFile);
      if (spineEntry.href == href) {
        spineIndex = static_cast<int16_t>(i);
        break;
      }
    }
    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  // Compose the title to NFC at index time so the cache stores precomposed glyphs;
  // device fonts have no combining-mark positioning, so NFD titles render broken.
  const TocEntry entry(utf8ComposeNfc(title), href, anchor, level, spineIndex);
  if (writeTocEntry(tocFile, entry) == UINT32_MAX) {
    LOG_ERR("BMC", "Failed to write TOC entry");
    return;
  }
  ++tocCount;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::load() {
  loaded = false;
  coreMetadata = {};
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  const auto fail = [this](const char* reason) {
    LOG_ERR("BMC", "Invalid metadata cache: %s", reason);
    loaded = false;
    bookFile.close();
    return false;
  };

  bookFileSize = bookFile.fileSize64();
  constexpr uint64_t MIN_HEADER_SIZE =
      sizeof(uint8_t) + sizeof(uint64_t) * 2 + sizeof(uint32_t) + sizeof(uint16_t) * 2 + sizeof(uint32_t) * 5;
  if (bookFileSize < MIN_HEADER_SIZE) return fail("truncated header");

  uint8_t version = 0;
  uint64_t cachedSourceSize = 0;
  uint64_t cachedSourceHash = 0;
  if (!serialization::readPod(bookFile, version)) return fail("missing version");
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    return fail("version mismatch");
  }
  if (!serialization::readPod(bookFile, cachedSourceSize) || !serialization::readPod(bookFile, cachedSourceHash) ||
      !serialization::readPod(bookFile, lutOffset) || !serialization::readPod(bookFile, spineCount) ||
      !serialization::readPod(bookFile, tocCount))
    return fail("truncated fixed header");
  if (spineCount == 0 || spineCount > MAX_SPINE_ENTRIES || tocCount > MAX_TOC_ENTRIES)
    return fail("entry count out of range");

  cache_integrity::SourceFingerprint currentSource;
  if (!cache_integrity::fingerprintFile(sourcePath, currentSource) || currentSource.size != cachedSourceSize ||
      currentSource.sampleHash != cachedSourceHash)
    return fail("source book changed");

  if (!serialization::readString(bookFile, coreMetadata.title) ||
      !serialization::readString(bookFile, coreMetadata.author) ||
      !serialization::readString(bookFile, coreMetadata.language) ||
      !serialization::readString(bookFile, coreMetadata.coverItemHref) ||
      !serialization::readString(bookFile, coreMetadata.textReferenceHref))
    return fail("truncated metadata");

  if (bookFile.position() != lutOffset) return fail("invalid LUT offset");
  const uint64_t lutEntries = static_cast<uint64_t>(spineCount) + tocCount;
  const uint64_t lutEnd = static_cast<uint64_t>(lutOffset) + lutEntries * sizeof(uint32_t);
  if (lutEnd > bookFileSize) return fail("LUT exceeds file");

  uint32_t previousPosition = 0;
  for (uint64_t i = 0; i < lutEntries; ++i) {
    const uint64_t lutPosition = static_cast<uint64_t>(lutOffset) + i * sizeof(uint32_t);
    if (!bookFile.seek64(lutPosition)) return fail("cannot seek LUT");
    uint32_t entryPosition = 0;
    if (!serialization::readPod(bookFile, entryPosition) || entryPosition < lutEnd || entryPosition >= bookFileSize ||
        (i > 0 && entryPosition < previousPosition))
      return fail("invalid entry offset");
    previousPosition = entryPosition;
    if (!bookFile.seek64(entryPosition)) return fail("cannot seek entry");
    if (i < spineCount) {
      const SpineEntry entry = readSpineEntry(bookFile);
      if (entry.href.empty() || (entry.tocIndex < -1 || entry.tocIndex >= static_cast<int16_t>(tocCount)))
        return fail("invalid spine entry");
    } else {
      const TocEntry entry = readTocEntry(bookFile);
      if (entry.href.empty() || entry.spineIndex < -1 || entry.spineIndex >= static_cast<int16_t>(spineCount))
        return fail("invalid TOC entry");
    }
    if (bookFile.position() > bookFileSize) return fail("entry exceeds file");
  }

  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
    return {};
  }

  // Seek to spine LUT item, read from LUT and get out data
  if (!bookFile.seek64(lutOffset + sizeof(uint32_t) * static_cast<uint64_t>(index))) return {};
  uint32_t spineEntryPos = 0;
  if (!serialization::readPod(bookFile, spineEntryPos) || spineEntryPos >= bookFileSize ||
      !bookFile.seek64(spineEntryPos))
    return {};
  return readSpineEntry(bookFile);
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_ERR("BMC", "getTocEntry index %d out of range", index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  const uint64_t tocLutPosition = static_cast<uint64_t>(lutOffset) + sizeof(uint32_t) * spineCount +
                                  sizeof(uint32_t) * static_cast<uint64_t>(index);
  if (!bookFile.seek64(tocLutPosition)) return {};
  uint32_t tocEntryPos = 0;
  if (!serialization::readPod(bookFile, tocEntryPos) || tocEntryPos >= bookFileSize || !bookFile.seek64(tocEntryPos))
    return {};
  return readTocEntry(bookFile);
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(HalFile& file) const {
  SpineEntry entry;
  if (!serialization::readString(file, entry.href) || !serialization::readPod(file, entry.cumulativeSize) ||
      !serialization::readPod(file, entry.tocIndex)) {
    LOG_ERR("BMC", "Truncated spine entry in metadata cache");
    return SpineEntry{};
  }
  return entry;
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(HalFile& file) const {
  TocEntry entry;
  if (!serialization::readString(file, entry.title) || !serialization::readString(file, entry.href) ||
      !serialization::readString(file, entry.anchor) || !serialization::readPod(file, entry.level) ||
      !serialization::readPod(file, entry.spineIndex)) {
    LOG_ERR("BMC", "Truncated TOC entry in metadata cache");
    return TocEntry{};
  }
  // level is 1-based in the on-disk format and indexes indentation arithmetic in
  // the chapter list. A corrupt 0 would underflow that calculation.
  if (entry.level == 0) {
    entry.level = 1;
  }
  return entry;
}

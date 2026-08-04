#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// Low-memory reader for an uncompressed StarDict set (.ifo + .idx + .dict).
// The complete .idx never lives in RAM: one checkpoint is retained for every
// 256 entries and lookups scan only the matching bracket.
class StarDictLookup {
 public:
  StarDictLookup() = default;
  ~StarDictLookup() { close(); }

  StarDictLookup(const StarDictLookup&) = delete;
  StarDictLookup& operator=(const StarDictLookup&) = delete;

  bool open(const std::string& folderPath);
  void close();
  bool isOpen() const { return isOpen_; }
  const std::string& bookname() const { return bookname_; }

  static constexpr uint32_t MAX_DEFINITION_BYTES = 4000;
  bool lookup(const std::string& queryWord, std::string& outDefinition, bool* outTruncated = nullptr);

 private:
  struct Checkpoint {
    Checkpoint(uint32_t offset, std::string text) : idxOffset(offset), entryText(std::move(text)) {}
    uint32_t idxOffset;
    std::string entryText;
  };

  bool parseIfo(const std::string& path);
  bool buildCheckpoints();
  bool readIdxEntryAt(uint32_t idxOffset, std::string& word, uint64_t& dictOffset, uint32_t& dictSize,
                      uint32_t& nextOffset);
  bool lookupViaCheckpoints(const std::string& candidate, uint64_t& dictOffset, uint32_t& dictSize);
  bool lookupViaLinearScan(const std::string& candidateLower, uint64_t& dictOffset, uint32_t& dictSize);
  bool decodeDefinitionData(const std::string& raw, std::string& decoded) const;

  bool isOpen_ = false;
  HalFile idxFile_;
  HalFile dictFile_;
  std::string bookname_;
  std::string sameTypeSequence_;
  uint32_t wordCount_ = 0;
  uint32_t idxFileSize_ = 0;
  bool use64BitOffsets_ = false;
  std::vector<Checkpoint> checkpoints_;
  static constexpr uint32_t CHECKPOINT_STRIDE = 256;
};

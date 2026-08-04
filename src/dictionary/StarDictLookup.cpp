#include "StarDictLookup.h"

#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

uint32_t readBe32(HalFile& file, bool& ok) {
  uint8_t b[4]{};
  ok = file.read(b, sizeof(b)) == static_cast<int>(sizeof(b));
  return ok ? (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                  (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3])
            : 0;
}

uint64_t readBe64(HalFile& file, bool& ok) {
  uint8_t b[8]{};
  ok = file.read(b, sizeof(b)) == static_cast<int>(sizeof(b));
  uint64_t value = 0;
  if (ok) {
    for (uint8_t byte : b) value = (value << 8) | byte;
  }
  return value;
}

bool readCString(HalFile& file, std::string& out) {
  out.clear();
  char c = 0;
  while (out.size() <= 256 && file.read(&c, 1) == 1) {
    if (c == '\0') return true;
    out.push_back(c);
  }
  return false;
}

bool endsWithIgnoreCase(const std::string& value, const char* suffix) {
  const size_t suffixLen = strlen(suffix);
  if (value.size() < suffixLen) return false;
  const size_t start = value.size() - suffixLen;
  for (size_t i = 0; i < suffixLen; ++i) {
    if (std::tolower(static_cast<unsigned char>(value[start + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

std::string lowerCopy(const std::string& input) {
  std::string out = input;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

std::string titleCopy(const std::string& input) {
  std::string out = lowerCopy(input);
  if (!out.empty()) out.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(out.front())));
  return out;
}

// Keep UTF-8 bytes. ASCII punctuation is stripped, while non-ASCII leading and
// trailing bytes remain part of the word and can be looked up verbatim.
std::string stripPunctuation(const std::string& input) {
  size_t begin = 0;
  size_t end = input.size();
  const auto keep = [](unsigned char c) { return c >= 0x80 || std::isalnum(c) || c == '\'' || c == '-'; };
  while (begin < end && !keep(static_cast<unsigned char>(input[begin]))) ++begin;
  while (end > begin && !keep(static_cast<unsigned char>(input[end - 1]))) --end;
  return input.substr(begin, end - begin);
}

std::vector<std::string> stemCandidates(const std::string& lower) {
  std::vector<std::string> out;
  const size_t n = lower.size();
  const auto add = [&out](std::string value) {
    if (value.size() >= 2 && std::find(out.begin(), out.end(), value) == out.end()) out.push_back(std::move(value));
  };
  if (n > 2 && lower.compare(n - 2, 2, "'s") == 0) add(lower.substr(0, n - 2));
  if (n > 4 && lower.compare(n - 3, 3, "ing") == 0) {
    const std::string base = lower.substr(0, n - 3);
    add(base);
    add(base + "e");
    if (base.size() >= 3 && base[base.size() - 1] == base[base.size() - 2]) add(base.substr(0, base.size() - 1));
  }
  if (n > 3 && lower.compare(n - 3, 3, "ied") == 0) add(lower.substr(0, n - 3) + "y");
  if (n > 3 && lower.compare(n - 2, 2, "ed") == 0) {
    const std::string base = lower.substr(0, n - 2);
    add(base);
    add(base + "e");
    if (base.size() >= 3 && base[base.size() - 1] == base[base.size() - 2]) add(base.substr(0, base.size() - 1));
  }
  if (n > 4 && lower.compare(n - 3, 3, "ies") == 0) add(lower.substr(0, n - 3) + "y");
  if (n > 3 && lower.compare(n - 2, 2, "es") == 0) add(lower.substr(0, n - 2));
  if (n > 2 && lower.back() == 's' && lower[n - 2] != 's') add(lower.substr(0, n - 1));
  return out;
}

}  // namespace

void StarDictLookup::close() {
  // close() is intentionally safe before open(), after a partial open and on
  // repeated activity teardown.  Guarding the handles also documents that
  // these two files are independently acquired.
  if (idxFile_) idxFile_.close();
  if (dictFile_) dictFile_.close();
  std::vector<Checkpoint>().swap(checkpoints_);
  std::string().swap(bookname_);
  std::string().swap(sameTypeSequence_);
  wordCount_ = 0;
  idxFileSize_ = 0;
  use64BitOffsets_ = false;
  isOpen_ = false;
}

bool StarDictLookup::parseIfo(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("DICT", path, file)) return false;
  // A valid .ifo is a short text header. Reject absurd input rather than
  // feeding a renamed book into an unbounded allocation.
  const size_t size = file.fileSize();
  if (size == 0 || size > 16384) return false;
  std::string contents(size, '\0');
  if (file.read(contents.data(), size) != static_cast<int>(size)) return false;
  size_t start = 0;
  while (start < contents.size()) {
    const size_t end = contents.find('\n', start);
    std::string line = contents.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? contents.size() : end + 1;
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    size_t first = 0;
    while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) ++first;
    const size_t equals = line.find('=', first);
    if (equals == std::string::npos || equals == first) continue;
    const std::string key = line.substr(first, equals - first);
    std::string value = line.substr(equals + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    if (key == "bookname")
      bookname_ = value;
    else if (key == "wordcount")
      wordCount_ = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
    else if (key == "idxfilesize")
      idxFileSize_ = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
    else if (key == "sametypesequence")
      sameTypeSequence_ = value;
    else if (key == "idxoffsetbits")
      use64BitOffsets_ = strtoul(value.c_str(), nullptr, 10) == 64;
  }
  return true;
}

bool StarDictLookup::open(const std::string& folderPath) {
  close();
  std::string ifoPath;
  std::string idxPath;
  std::string dictPath;
  for (const auto& entry : Storage.listFiles(folderPath.c_str())) {
    const std::string name = entry.c_str();
    if (name.empty() || name.front() == '.') continue;
    const std::string full = folderPath + "/" + name;
    if (endsWithIgnoreCase(name, ".ifo"))
      ifoPath = full;
    else if (endsWithIgnoreCase(name, ".idx"))
      idxPath = full;
    else if (endsWithIgnoreCase(name, ".dict"))
      dictPath = full;
  }
  if (ifoPath.empty() || idxPath.empty() || dictPath.empty()) {
    LOG_ERR("DICT", "Missing .ifo/.idx/.dict in %s", folderPath.c_str());
    return false;
  }
  if (!parseIfo(ifoPath) || !Storage.openFileForRead("DICT", idxPath, idxFile_) ||
      !Storage.openFileForRead("DICT", dictPath, dictFile_)) {
    close();
    return false;
  }
  idxFileSize_ = static_cast<uint32_t>(idxFile_.fileSize());
  if (idxFileSize_ == 0 || !buildCheckpoints()) {
    close();
    return false;
  }
  isOpen_ = true;
  LOG_INF("DICT", "Opened %s: %u words, %u checkpoints", bookname_.c_str(), wordCount_,
          static_cast<unsigned>(checkpoints_.size()));
  return true;
}

bool StarDictLookup::readIdxEntryAt(const uint32_t idxOffset, std::string& word, uint64_t& dictOffset,
                                    uint32_t& dictSize, uint32_t& nextOffset) {
  if (!idxFile_.seekSet(idxOffset) || !readCString(idxFile_, word)) return false;
  bool ok = false;
  dictOffset = use64BitOffsets_ ? readBe64(idxFile_, ok) : static_cast<uint64_t>(readBe32(idxFile_, ok));
  if (!ok) return false;
  dictSize = readBe32(idxFile_, ok);
  if (!ok) return false;
  nextOffset = static_cast<uint32_t>(idxFile_.position());
  return nextOffset > idxOffset && nextOffset <= idxFileSize_;
}

bool StarDictLookup::buildCheckpoints() {
  uint32_t offset = 0;
  uint32_t count = 0;
  while (offset < idxFileSize_) {
    std::string word;
    uint64_t dictOffset = 0;
    uint32_t dictSize = 0;
    uint32_t nextOffset = 0;
    if (!readIdxEntryAt(offset, word, dictOffset, dictSize, nextOffset)) return false;
    if ((count % CHECKPOINT_STRIDE) == 0) checkpoints_.emplace_back(offset, word);
    offset = nextOffset;
    ++count;
  }
  if (wordCount_ == 0) wordCount_ = count;
  return !checkpoints_.empty();
}

bool StarDictLookup::lookupViaCheckpoints(const std::string& candidate, uint64_t& dictOffset, uint32_t& dictSize) {
  if (checkpoints_.empty()) return false;
  size_t low = 0;
  size_t high = checkpoints_.size();
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (checkpoints_[mid].entryText.compare(candidate) <= 0)
      low = mid + 1;
    else
      high = mid;
  }
  // A query before the first checkpoint can still match the first bracket.
  const size_t bracket = low == 0 ? 0 : low - 1;
  uint32_t offset = checkpoints_[bracket].idxOffset;
  const uint32_t end = bracket + 1 < checkpoints_.size() ? checkpoints_[bracket + 1].idxOffset : idxFileSize_;
  while (offset < end) {
    std::string word;
    uint32_t next = 0;
    if (!readIdxEntryAt(offset, word, dictOffset, dictSize, next)) break;
    const int cmp = word.compare(candidate);
    if (cmp == 0) return true;
    if (cmp > 0) break;
    offset = next;
  }
  return false;
}

bool StarDictLookup::lookupViaLinearScan(const std::string& candidateLower, uint64_t& dictOffset, uint32_t& dictSize) {
  uint32_t offset = 0;
  while (offset < idxFileSize_) {
    std::string word;
    uint32_t next = 0;
    if (!readIdxEntryAt(offset, word, dictOffset, dictSize, next)) break;
    if (lowerCopy(word) == candidateLower) return true;
    offset = next;
  }
  return false;
}

bool StarDictLookup::decodeDefinitionData(const std::string& raw, std::string& decoded) const {
  decoded.clear();
  size_t pos = 0;
  size_t sequencePos = 0;
  const bool hasSequence = !sameTypeSequence_.empty();
  while (pos < raw.size()) {
    char type = 0;
    if (hasSequence) {
      if (sequencePos >= sameTypeSequence_.size()) break;
      type = sameTypeSequence_[sequencePos++];
    } else {
      type = raw[pos++];
    }

    size_t fieldStart = pos;
    size_t fieldSize = 0;
    if (type >= 'a' && type <= 'z') {
      // With sametypesequence the final string field consumes the remaining
      // bytes and is not required to carry a trailing NUL.
      if (hasSequence && sequencePos == sameTypeSequence_.size()) {
        fieldSize = raw.size() - pos;
        pos = raw.size();
      } else {
        const size_t terminator = raw.find('\0', pos);
        if (terminator == std::string::npos) return false;
        fieldSize = terminator - pos;
        pos = terminator + 1;
      }
    } else if (type >= 'A' && type <= 'Z') {
      if (pos + 4 > raw.size()) return false;
      fieldSize = (static_cast<uint32_t>(static_cast<unsigned char>(raw[pos])) << 24) |
                  (static_cast<uint32_t>(static_cast<unsigned char>(raw[pos + 1])) << 16) |
                  (static_cast<uint32_t>(static_cast<unsigned char>(raw[pos + 2])) << 8) |
                  static_cast<uint32_t>(static_cast<unsigned char>(raw[pos + 3]));
      pos += 4;
      fieldStart = pos;
      if (fieldSize > raw.size() - pos) return false;
      pos += fieldSize;
    } else {
      return false;
    }

    // Lowercase StarDict fields are textual. Uppercase fields are binary
    // payloads (audio/images/resources) and must never be sent to the font or
    // HTML parsers. 'r' is a textual resource reference, useful to show when a
    // dictionary does not provide an inline definition.
    if (type >= 'a' && type <= 'z' && fieldSize > 0) {
      if (!decoded.empty()) decoded.push_back('\n');
      decoded.append(raw, fieldStart, fieldSize);
    }
  }
  return !decoded.empty();
}

bool StarDictLookup::lookup(const std::string& queryWord, std::string& outDefinition, bool* outTruncated) {
  outDefinition.clear();
  if (outTruncated) *outTruncated = false;
  if (!isOpen_) return false;
  const std::string cleaned = stripPunctuation(queryWord);
  if (cleaned.empty()) return false;

  const std::string lower = lowerCopy(cleaned);
  std::vector<std::string> candidates{cleaned, lower, titleCopy(cleaned)};
  for (const std::string& stem : stemCandidates(lower)) {
    candidates.push_back(stem);
    candidates.push_back(titleCopy(stem));
  }

  uint64_t dictOffset = 0;
  uint32_t dictSize = 0;
  bool found = false;
  for (const std::string& candidate : candidates) {
    if (lookupViaCheckpoints(candidate, dictOffset, dictSize)) {
      found = true;
      break;
    }
  }
  if (!found) {
    std::vector<std::string> lowerCandidates{lower};
    const auto stems = stemCandidates(lower);
    lowerCandidates.insert(lowerCandidates.end(), stems.begin(), stems.end());
    for (const std::string& candidate : lowerCandidates) {
      if (lookupViaLinearScan(candidate, dictOffset, dictSize)) {
        found = true;
        break;
      }
    }
  }
  if (!found || dictSize == 0 || !dictFile_.seek64(dictOffset)) return false;

  const uint32_t readSize = std::min(dictSize, MAX_DEFINITION_BYTES);
  std::string raw(readSize, '\0');
  if (dictFile_.read(raw.data(), readSize) != static_cast<int>(readSize)) {
    return false;
  }
  // A malformed type layout should not make an otherwise readable legacy
  // dictionary unusable. Fall back to the raw field, matching inx's behavior.
  if (!decodeDefinitionData(raw, outDefinition)) outDefinition = std::move(raw);
  if (outTruncated) *outTruncated = readSize < dictSize;
  return true;
}

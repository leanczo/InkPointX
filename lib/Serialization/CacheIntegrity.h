#pragma once

#include <HalStorage.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cache_integrity {

inline constexpr uint64_t FNV1A_OFFSET_BASIS = 14695981039346656037ull;
inline constexpr uint64_t FNV1A_PRIME = 1099511628211ull;

inline uint64_t mixBytes(uint64_t hash, const void* data, const size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= FNV1A_PRIME;
  }
  return hash;
}

// std::hash is intentionally implementation-defined. Cache/progress paths must
// stay stable across compiler upgrades, so new formats use this explicit hash.
inline uint64_t stablePathHash(const std::string_view value) {
  return mixBytes(FNV1A_OFFSET_BASIS, value.data(), value.size());
}

struct SourceFingerprint {
  uint64_t size = 0;
  uint64_t sampleHash = 0;

  bool operator==(const SourceFingerprint& other) const { return size == other.size && sampleHash == other.sampleHash; }
};

// Hash bounded samples from the beginning, middle and end of a book. This
// catches same-size replacements without adding a full sequential read to every
// book open (important on slow SD cards). Offsets are mixed into the hash so
// identical chunks at different positions cannot cancel one another.
inline bool fingerprintFile(const std::string& path, SourceFingerprint& result) {
  HalFile file;
  if (!Storage.openFileForRead("CFP", path, file)) return false;

  result = {};
  result.size = file.fileSize64();
  uint64_t hash = mixBytes(FNV1A_OFFSET_BASIS, &result.size, sizeof(result.size));

  constexpr uint64_t SAMPLE_BYTES = 1024;
  std::array<uint64_t, 3> offsets = {
      0,
      result.size > SAMPLE_BYTES ? (result.size - std::min<uint64_t>(result.size, SAMPLE_BYTES)) / 2 : 0,
      result.size > SAMPLE_BYTES ? result.size - SAMPLE_BYTES : 0,
  };
  uint8_t buffer[256];
  uint64_t previousOffset = UINT64_MAX;
  for (const uint64_t offset : offsets) {
    if (offset == previousOffset) continue;
    previousOffset = offset;
    if (!file.seek64(offset)) {
      file.close();
      return false;
    }
    hash = mixBytes(hash, &offset, sizeof(offset));
    const uint64_t available = result.size - offset;
    size_t remaining = static_cast<size_t>(std::min<uint64_t>(SAMPLE_BYTES, available));
    while (remaining > 0) {
      const size_t requested = std::min(remaining, sizeof(buffer));
      const int read = file.read(buffer, requested);
      if (read != static_cast<int>(requested)) {
        file.close();
        return false;
      }
      hash = mixBytes(hash, buffer, requested);
      remaining -= requested;
    }
  }
  file.close();
  result.sampleHash = hash;
  return true;
}

}  // namespace cache_integrity

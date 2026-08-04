#pragma once
#include <HalStorage.h>

#include <iostream>

namespace serialization {

// Upper bound for any string read back from storage. These files hold titles,
// author names, chapter labels and paths, so a few hundred bytes is the real
// ceiling. The length prefix is a raw uint32_t off the SD card: a file
// truncated by a card yank or power loss can present up to 4 GiB, and with
// -fno-exceptions the failed resize() aborts the firmware instead of raising.
// Because the affected files are read while opening a book and during boot,
// that abort becomes a reboot loop that survives a power cycle.
inline constexpr uint32_t MAX_SERIALIZED_STRING_LENGTH = 4096;

template <typename T>
bool writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
  return static_cast<bool>(os);
}

template <typename T>
bool writePod(HalFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

template <typename T>
bool readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
  return static_cast<bool>(is);
}

// Returns false on a short read so callers can reject a truncated file instead
// of continuing with whatever the destination happened to hold.
template <typename T>
bool readPod(HalFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == static_cast<int>(sizeof(T));
}

inline bool writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  if (!writePod(os, len)) return false;
  os.write(s.data(), len);
  return static_cast<bool>(os);
}

inline bool writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  return writePod(file, len) && file.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len;
}

inline bool readString(std::istream& is, std::string& s) {
  uint32_t len = 0;
  s.clear();
  if (!readPod(is, len) || len > MAX_SERIALIZED_STRING_LENGTH) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  s.resize(len);
  is.read(s.data(), len);
  return static_cast<bool>(is);
}

inline bool readString(HalFile& file, std::string& s) {
  uint32_t len = 0;
  s.clear();
  if (!readPod(file, len) || len > MAX_SERIALIZED_STRING_LENGTH) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  s.resize(len);
  return file.read(&s[0], len) == static_cast<int>(len);
}
}  // namespace serialization

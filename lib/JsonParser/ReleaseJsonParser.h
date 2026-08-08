#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "StreamingJsonParser.h"

class ReleaseJsonParser {
 public:
  static constexpr size_t MAX_RELEASE_NOTES_SIZE = 3072;
  ReleaseJsonParser();

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  bool foundTag() const;
  bool foundFirmware() const;
  const char* getTagName() const;
  const char* getFirmwareUrl() const;
  const char* getFirmwareDigest() const;
  size_t getFirmwareSize() const;
  const std::string& getReleaseNotes() const;

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    RELEASE_NOTES,
    ASSETS,
    ASSET_NAME,
    ASSET_URL,
    ASSET_DIGEST,
    ASSET_SIZE,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);
  static void sOnStringChunk(void* ctx, const char* value, size_t len, bool final);

  void commitAsset();

  StreamingJsonParser parser;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;

  char tagName[32];
  char firmwareUrl[512];
  char firmwareDigest[80];
  size_t firmwareSize;
  bool tagFound;
  bool firmwareFound;
  std::string releaseNotes;

  char currentAssetName[32];
  char currentAssetUrl[512];
  char currentAssetDigest[80];
  size_t currentAssetSize;
};

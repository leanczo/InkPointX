#include "SleepImageInstaller.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include <cstdint>

namespace {
constexpr const char* TEMP_IMAGE_PATH = "/.sleep-install.tmp";
constexpr const char* BACKUP_IMAGE_PATH = "/.sleep-install.bak";

bool validateBmp(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("SLPIMG", path, file)) return false;

  bool valid = false;
  {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      const uint64_t requiredSize = static_cast<uint64_t>(file.position()) +
                                    static_cast<uint64_t>(bitmap.getRowBytes()) * bitmap.getHeight();
      valid = file.fileSize64() >= requiredSize;
    }
  }
  file.close();
  return valid;
}

bool copyBmp(HalFile& source, HalFile& destination) {
  uint8_t buffer[2048];
  while (true) {
    const int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead < 0) return false;
    if (bytesRead == 0) return true;
    if (destination.write(buffer, static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) return false;
  }
}
}  // namespace

namespace SleepImageInstaller {

bool install(const std::string& sourcePath, const bool crop) {
  const bool isBmp = FsHelpers::hasBmpExtension(sourcePath);
  const bool isJpeg = FsHelpers::hasJpgExtension(sourcePath);
  const bool isPng = FsHelpers::hasPngExtension(sourcePath);
  if (!isBmp && !isJpeg && !isPng) return false;

  if (sourcePath == INSTALLED_IMAGE_PATH) return isBmp && validateBmp(INSTALLED_IMAGE_PATH);

  if (Storage.exists(TEMP_IMAGE_PATH)) Storage.remove(TEMP_IMAGE_PATH);

  HalFile source;
  if (!Storage.openFileForRead("SLPIMG", sourcePath, source)) return false;

  HalFile destination;
  if (!Storage.openFileForWrite("SLPIMG", TEMP_IMAGE_PATH, destination)) {
    source.close();
    return false;
  }

  bool converted = false;
  if (isBmp) {
    converted = copyBmp(source, destination);
  } else if (isJpeg) {
    converted = JpegToBmpConverter::jpegFileToBmpStream(source, destination, crop);
  } else {
    converted = PngToBmpConverter::pngFileToBmpStream(source, destination, crop);
  }
  source.close();
  destination.close();

  if (!converted || !validateBmp(TEMP_IMAGE_PATH)) {
    Storage.remove(TEMP_IMAGE_PATH);
    LOG_ERR("SLPIMG", "Unable to convert or validate %s", sourcePath.c_str());
    return false;
  }

  // Replace the installed image transactionally. If the final rename fails,
  // restore the previous lock-screen image instead of leaving no valid image.
  if (Storage.exists(BACKUP_IMAGE_PATH) && !Storage.remove(BACKUP_IMAGE_PATH)) {
    Storage.remove(TEMP_IMAGE_PATH);
    return false;
  }

  const bool hadPreviousImage = Storage.exists(INSTALLED_IMAGE_PATH);
  if (hadPreviousImage && !Storage.rename(INSTALLED_IMAGE_PATH, BACKUP_IMAGE_PATH)) {
    Storage.remove(TEMP_IMAGE_PATH);
    return false;
  }

  if (!Storage.rename(TEMP_IMAGE_PATH, INSTALLED_IMAGE_PATH)) {
    if (hadPreviousImage) Storage.rename(BACKUP_IMAGE_PATH, INSTALLED_IMAGE_PATH);
    Storage.remove(TEMP_IMAGE_PATH);
    return false;
  }

  if (hadPreviousImage) Storage.remove(BACKUP_IMAGE_PATH);
  return true;
}

}  // namespace SleepImageInstaller

#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdint>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  // Verify dimensions are close (allow 1 pixel tolerance for rounding differences)
  int widthDiff = abs(cachedWidth - expectedWidth);
  int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    LOG_ERR("IMG", "Cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
            expectedHeight);
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

bool renderViewportFromCache(GfxRenderer& renderer, const std::string& cachePath, const int x, const int y,
                             const int expectedWidth, const int expectedHeight, int sourceX, int sourceY,
                             int sourceWidth, int sourceHeight, const int destinationWidth,
                             const int destinationHeight) {
  if (destinationWidth <= 0 || destinationHeight <= 0 || x < 0 || y < 0 ||
      x + destinationWidth > renderer.getScreenWidth() || y + destinationHeight > renderer.getScreenHeight()) {
    LOG_ERR("IMG", "Invalid viewport destination: (%d,%d) size (%dx%d)", x, y, destinationWidth, destinationHeight);
    return false;
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) return false;

  uint16_t cachedWidth = 0;
  uint16_t cachedHeight = 0;
  if (cacheFile.read(&cachedWidth, sizeof(cachedWidth)) != sizeof(cachedWidth) ||
      cacheFile.read(&cachedHeight, sizeof(cachedHeight)) != sizeof(cachedHeight)) {
    return false;
  }
  if (abs(static_cast<int>(cachedWidth) - expectedWidth) > 1 ||
      abs(static_cast<int>(cachedHeight) - expectedHeight) > 1) {
    LOG_ERR("IMG", "Viewport cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
            expectedHeight);
    return false;
  }

  sourceX = std::clamp(sourceX, 0, static_cast<int>(cachedWidth) - 1);
  sourceY = std::clamp(sourceY, 0, static_cast<int>(cachedHeight) - 1);
  sourceWidth = std::clamp(sourceWidth, 1, static_cast<int>(cachedWidth) - sourceX);
  sourceHeight = std::clamp(sourceHeight, 1, static_cast<int>(cachedHeight) - sourceY);

  // Grayscale strip passes can reject the complete logical destination before
  // touching the SD card. The PDF zoom path is normally BW-only, but keeping
  // this generic makes the primitive safe for other image callers.
  if (!renderer.glyphIntersectsStrip(x, y, x + destinationWidth - 1, y + destinationHeight - 1)) return true;

  const int bytesPerRow = (cachedWidth + 3) / 4;
  int rowsPerRead = std::max(1, 4096 / bytesPerRow);
  rowsPerRead = std::min(rowsPerRead, static_cast<int>(cachedHeight));
  auto* readBuffer = static_cast<uint8_t*>(malloc(static_cast<size_t>(rowsPerRead) * bytesPerRow));
  if (!readBuffer) {
    rowsPerRead = 1;
    readBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate PDF viewport row buffer");
    return false;
  }

  DirectPixelWriter writer;
  writer.init(renderer);
  int bufferedStart = -1;
  int bufferedRows = 0;
  bool success = true;

  for (int destinationY = 0; destinationY < destinationHeight; ++destinationY) {
    const int sourceRow =
        sourceY + std::min(sourceHeight - 1,
                           static_cast<int>((static_cast<int64_t>(destinationY) * sourceHeight) / destinationHeight));
    if (sourceRow < bufferedStart || sourceRow >= bufferedStart + bufferedRows) {
      bufferedStart = sourceRow;
      bufferedRows = std::min(rowsPerRead, static_cast<int>(cachedHeight) - bufferedStart);
      const uint64_t offset = sizeof(cachedWidth) + sizeof(cachedHeight) +
                              static_cast<uint64_t>(bufferedStart) * static_cast<uint64_t>(bytesPerRow);
      const size_t bytes = static_cast<size_t>(bufferedRows) * bytesPerRow;
      if (!cacheFile.seek64(offset) || cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Viewport cache read error at source row %d", sourceRow);
        success = false;
        break;
      }
    }

    const uint8_t* rowBuffer = readBuffer + static_cast<size_t>(sourceRow - bufferedStart) * bytesPerRow;
    writer.beginRow(y + destinationY);
    int destinationStart = 0;
    int destinationEnd = destinationWidth;
    writer.bandColRange(x, destinationWidth, destinationStart, destinationEnd);
    for (int destinationX = destinationStart; destinationX < destinationEnd; ++destinationX) {
      const int sourceColumn =
          sourceX + std::min(sourceWidth - 1,
                             static_cast<int>((static_cast<int64_t>(destinationX) * sourceWidth) / destinationWidth));
      const int byteIndex = sourceColumn >> 2;
      const int bitShift = 6 - (sourceColumn & 3) * 2;
      writer.writePixel(x + destinationX, (rowBuffer[byteIndex] >> bitShift) & 0x03);
    }
  }

  free(readBuffer);
  return success;
}

}  // namespace

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::renderViewport(GfxRenderer& renderer, const int x, const int y, const int sourceX, const int sourceY,
                                const int sourceWidth, const int sourceHeight, const int destinationWidth,
                                const int destinationHeight) {
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return true;

  const std::string cachePath = getCachePath(imagePath);
  if (renderViewportFromCache(renderer, cachePath, x, y, width, height, sourceX, sourceY, sourceWidth, sourceHeight,
                              destinationWidth, destinationHeight)) {
    return true;
  }

  // The first normal page view usually created the cache already. If it did
  // not (cache removed while the book was open, first page opened directly in
  // zoom, etc.), run the regular decoder once and immediately retry the crop.
  render(renderer, x, y);
  // The decoder also painted the fit-page image. Clear those pixels before the
  // cropped render; otherwise black marks outside the selected source area
  // survive because DirectPixelWriter intentionally skips white pixels.
  renderer.fillRect(x, y, destinationWidth, destinationHeight, false);
  return renderViewportFromCache(renderer, cachePath, x, y, width, height, sourceX, sourceY, sourceWidth, sourceHeight,
                                 destinationWidth, destinationHeight);
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  int16_t w = 0, h = 0;
  if (!serialization::readString(file, path) || path.empty() || !serialization::readPod(file, w) ||
      !serialization::readPod(file, h) || w <= 0 || h <= 0 || w > 2048 || h > 2048)
    return nullptr;
  return std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, w, h));
}

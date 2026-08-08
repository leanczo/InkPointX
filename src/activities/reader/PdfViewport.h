#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// Geometry for the PDF reader's tiled zoom mode.  The full-page image is
// already cached at the panel's native resolution; zooming selects a smaller
// source rectangle and expands it back into the same on-screen image bounds.
// Adjacent rectangles are spread from edge to edge, so fractional zoom levels
// overlap instead of leaving an unread strip between tiles.
struct PdfViewportRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int columns = 1;
  int rows = 1;
  int index = 0;

  [[nodiscard]] int count() const { return columns * rows; }
};

class PdfViewport {
 public:
  static constexpr std::array<uint16_t, 4> ZOOM_PERCENT = {100, 125, 150, 200};

  [[nodiscard]] static uint8_t clampOption(const uint8_t option) {
    return std::min<uint8_t>(option, static_cast<uint8_t>(ZOOM_PERCENT.size() - 1));
  }

  [[nodiscard]] static uint16_t zoomPercent(const uint8_t option) { return ZOOM_PERCENT[clampOption(option)]; }

  [[nodiscard]] static PdfViewportRect calculate(const int imageWidth, const int imageHeight, const uint8_t zoomOption,
                                                 const int requestedIndex) {
    PdfViewportRect result;
    if (imageWidth <= 0 || imageHeight <= 0) return result;

    const int zoom = zoomPercent(zoomOption);
    // Round up so every source pixel remains reachable at the far edge.
    result.width = std::max(1, (imageWidth * 100 + zoom - 1) / zoom);
    result.height = std::max(1, (imageHeight * 100 + zoom - 1) / zoom);
    result.columns = std::max(1, (imageWidth + result.width - 1) / result.width);
    result.rows = std::max(1, (imageHeight + result.height - 1) / result.height);

    const int viewCount = result.count();
    result.index = std::clamp(requestedIndex, 0, viewCount - 1);
    const int column = result.index % result.columns;
    const int row = result.index / result.columns;
    const int maxX = imageWidth - result.width;
    const int maxY = imageHeight - result.height;
    result.x = result.columns > 1 ? (column * maxX) / (result.columns - 1) : 0;
    result.y = result.rows > 1 ? (row * maxY) / (result.rows - 1) : 0;
    return result;
  }
};

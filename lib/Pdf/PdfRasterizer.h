#pragma once

#include <pdfio.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Small, monochrome PDF graphics renderer used for fixed-layout pages that the
// text-oriented PDF adapter cannot faithfully reflow (music, diagrams, forms,
// and other pages built from vector paths and embedded TrueType glyphs).
//
// The caller can lend the display framebuffer as scratch storage. Rendering is
// performed at the active device's portrait geometry and the resulting 1-bit
// PNG is cached on the SD card for the regular EPUB image pipeline.
class PdfRasterizer {
 public:
  PdfRasterizer(uint8_t* scratch, size_t scratchSize, uint16_t width, uint16_t height)
      : scratch(scratch), scratchSize(scratchSize), rasterWidth(width), maxRasterHeight(height) {}

  static bool pageNeedsRasterization(pdfio_obj_t* page);

  bool renderPage(pdfio_obj_t* page, const std::string& outputPath, std::string& error);

 private:
  uint8_t* scratch = nullptr;
  size_t scratchSize = 0;
  uint16_t rasterWidth = 480;
  uint16_t maxRasterHeight = 760;
};

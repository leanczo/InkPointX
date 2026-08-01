#include <HalStorage.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "Pdf.h"

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::cerr << "usage: pdf_native_harness PDF CACHE_BASE [x3|x4]\n";
    return 2;
  }
  const std::string device = argc == 4 ? argv[3] : "x4";
  if (device != "x3" && device != "x4") {
    std::cerr << "device must be x3 or x4\n";
    return 2;
  }
  const uint16_t rasterWidth = device == "x3" ? 528 : 480;
  const uint16_t rasterHeight = device == "x3" ? 752 : 760;
  const size_t frameBufferSize = device == "x3" ? 52272 : 48000;
  Storage.setRoot(std::filesystem::path("/"));
  Pdf pdf(argv[1], argv[2]);
  std::vector<uint8_t> rasterScratch(frameBufferSize);
  pdf.setRasterGeometry(rasterWidth, rasterHeight);
  pdf.setRasterScratch(rasterScratch.data(), rasterScratch.size());
  pdf.clearCache();
  if (!pdf.load()) {
    std::cerr << "PDF load failed: " << pdf.getLastError() << "\n";
    return 1;
  }
  std::cout << "title=" << pdf.getTitle() << "\n"
            << "author=" << pdf.getAuthor() << "\n"
            << "pages=" << pdf.getPageCount() << "\n"
            << "device=" << device << "\n"
            << "raster=" << rasterWidth << "x" << rasterHeight << "\n"
            << "package=" << pdf.getPackagePath() << "\n";
  return 0;
}

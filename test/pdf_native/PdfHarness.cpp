#include <HalStorage.h>

#include <filesystem>
#include <iostream>

#include "Pdf.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: pdf_native_harness PDF CACHE_BASE\n";
    return 2;
  }
  Storage.setRoot(std::filesystem::path("/"));
  Pdf pdf(argv[1], argv[2]);
  pdf.clearCache();
  if (!pdf.load()) {
    std::cerr << "PDF load failed: " << pdf.getLastError() << "\n";
    return 1;
  }
  std::cout << "title=" << pdf.getTitle() << "\n"
            << "author=" << pdf.getAuthor() << "\n"
            << "pages=" << pdf.getPageCount() << "\n"
            << "package=" << pdf.getPackagePath() << "\n";
  return 0;
}

cmake_minimum_required(VERSION 3.16)
project(x4_pdf_native_harness C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ZLIB REQUIRED)
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

set(PDFIO_SOURCES
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-aes.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-array.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-common.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-crypto.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-dict.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-file.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-md5.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-object.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-page.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-rc4.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-sha256.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-stream.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-string.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-token.c
  ${REPO_ROOT}/lib/Pdfio/src/pdfio-value.c
)
add_executable(pdf_native_harness
  PdfHarness.cpp
  ${REPO_ROOT}/lib/Pdf/Pdf.cpp
  ${REPO_ROOT}/lib/Pdf/PdfRasterizer.cpp
  ${PDFIO_SOURCES}
)
target_compile_definitions(pdf_native_harness PRIVATE PDFIO_READ_ONLY=1)
target_compile_options(pdf_native_harness PRIVATE -ffunction-sections -fdata-sections)
target_link_options(pdf_native_harness PRIVATE -Wl,-dead_strip)
target_link_libraries(pdf_native_harness PRIVATE ZLIB::ZLIB)
target_include_directories(pdf_native_harness BEFORE PRIVATE
  ${REPO_ROOT}/test/fb2_encoding/mocks
  ${REPO_ROOT}/lib/Pdf
  ${REPO_ROOT}/lib/Pdfio/src
)

add_executable(pdf_viewport_tests
  PdfViewportTest.cpp
)

target_link_libraries(pdf_viewport_tests PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(pdf_viewport_tests PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

gtest_discover_tests(pdf_viewport_tests)

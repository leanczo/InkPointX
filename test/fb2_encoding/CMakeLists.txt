find_package(EXPAT REQUIRED)

add_executable(fb2_encoding_tests
  Fb2EncodingTest.cpp
  Fb2ParserTest.cpp
  ${REPO_ROOT}/lib/Fb2/Fb2.cpp
  ${REPO_ROOT}/lib/Fb2/Fb2Encoding.cpp
)

target_link_libraries(fb2_encoding_tests PRIVATE
  crosspoint_test_common
  EXPAT::EXPAT
  GTest::gtest_main
)

target_include_directories(fb2_encoding_tests BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  ${REPO_ROOT}/lib/Fb2
  ${REPO_ROOT}/lib/Serialization
)

gtest_discover_tests(fb2_encoding_tests)

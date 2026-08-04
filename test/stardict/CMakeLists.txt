add_executable(StarDictLookupTest
  StarDictLookupTest.cpp
  ${REPO_ROOT}/src/dictionary/StarDictLookup.cpp
)
target_include_directories(StarDictLookupTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  ${REPO_ROOT}/src/dictionary
)
target_link_libraries(StarDictLookupTest PRIVATE GTest::gtest_main crosspoint_test_common)
gtest_discover_tests(StarDictLookupTest)

add_executable(ArabicTextTest
  ArabicTextTest.cpp
  ${REPO_ROOT}/lib/MiniBidi/ArabicShaper.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
)

target_include_directories(ArabicTextTest PRIVATE
  ${REPO_ROOT}/lib/MiniBidi
)

target_link_libraries(ArabicTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ArabicTextTest)

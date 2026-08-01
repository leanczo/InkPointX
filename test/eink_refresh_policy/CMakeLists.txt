add_executable(eink_refresh_policy_tests
  EInkRefreshPolicyTest.cpp
)

target_link_libraries(eink_refresh_policy_tests PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(eink_refresh_policy_tests)

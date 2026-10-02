add_executable(test_smoke smoke_test.cpp)
target_link_libraries(test_smoke PRIVATE lob_core GTest::gtest_main lob_warnings)
gtest_discover_tests(test_smoke)
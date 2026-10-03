add_executable(test_fuzz fuzz_test.cpp)
target_link_libraries(test_fuzz PRIVATE lob_fuzz_targets GTest::gtest_main lob_warnings)
gtest_discover_tests(test_fuzz)

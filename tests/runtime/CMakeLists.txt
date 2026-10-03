add_executable(test_runtime runtime_test.cpp)
target_link_libraries(test_runtime PRIVATE lob_runtime GTest::gtest_main lob_warnings)
gtest_discover_tests(test_runtime)

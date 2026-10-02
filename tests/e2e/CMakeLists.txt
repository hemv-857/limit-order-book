add_executable(test_e2e e2e_test.cpp)
target_link_libraries(test_e2e PRIVATE lob_venue GTest::gtest_main lob_warnings)
gtest_discover_tests(test_e2e)

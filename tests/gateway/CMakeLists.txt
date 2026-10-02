add_executable(test_gateway market_data_test.cpp session_test.cpp reactor_test.cpp)
target_link_libraries(test_gateway PRIVATE lob_gateway GTest::gtest_main lob_warnings)
gtest_discover_tests(test_gateway)

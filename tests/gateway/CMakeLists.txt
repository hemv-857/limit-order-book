add_executable(test_gateway market_data_test.cpp)
target_link_libraries(test_gateway PRIVATE lob_gateway GTest::gtest_main)
add_test(NAME gateway COMMAND test_gateway)

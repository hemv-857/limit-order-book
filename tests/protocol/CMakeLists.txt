add_executable(test_protocol codec_test.cpp)
target_link_libraries(test_protocol PRIVATE lob_protocol GTest::gtest_main)
add_test(NAME protocol COMMAND test_protocol)

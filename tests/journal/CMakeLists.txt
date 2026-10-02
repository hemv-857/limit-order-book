add_executable(test_journal journal_test.cpp)
target_link_libraries(test_journal PRIVATE lob_journal GTest::gtest_main lob_warnings)
gtest_discover_tests(test_journal)

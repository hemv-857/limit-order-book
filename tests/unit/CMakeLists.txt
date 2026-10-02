add_executable(test_smoke smoke_test.cpp)
target_link_libraries(test_smoke PRIVATE lob_core GTest::gtest_main lob_warnings)
gtest_discover_tests(test_smoke)
add_executable(test_book book_test.cpp)
target_link_libraries(test_book PRIVATE lob_core GTest::gtest_main lob_warnings)
gtest_discover_tests(test_book)

# alloc_guard.cpp defines global operator new/delete for the whole test binary,
# so it must live in exactly one target.
add_executable(test_no_alloc no_alloc_test.cpp alloc_guard.cpp)
target_link_libraries(test_no_alloc PRIVATE lob_core GTest::gtest_main lob_warnings)
gtest_discover_tests(test_no_alloc)

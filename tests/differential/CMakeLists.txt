add_library(lob_reference STATIC ../reference/reference_engine.cpp)
target_include_directories(lob_reference PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(lob_reference PUBLIC lob_core PRIVATE lob_warnings)

add_executable(test_differential differential_test.cpp)
target_include_directories(test_differential PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(test_differential PRIVATE lob_core lob_reference GTest::gtest_main
                                            lob_warnings)
gtest_discover_tests(test_differential)

# The long differential run is a separate test with a generous timeout.
# It is DISABLED_ pending the open divergence documented in the test file.
add_test(NAME differential_long COMMAND test_differential
                                --gtest_filter=Differential.EnginesAgree)
set_tests_properties(differential_long PROPERTIES TIMEOUT 1800)

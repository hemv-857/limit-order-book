add_library(lob_reference STATIC ../reference/reference_engine.cpp)
target_include_directories(lob_reference PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(lob_reference PUBLIC lob_core PRIVATE lob_warnings)

add_executable(test_differential differential_test.cpp)
target_include_directories(test_differential PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(test_differential PRIVATE lob_core lob_reference GTest::gtest_main
                                            lob_warnings)
gtest_discover_tests(test_differential)

# The long differential run: the same scenarios at a depth that can actually
# find a rare divergence. Without LOB_DIFF_OPS this re-ran the default depth,
# which made it a slower copy of Differential.EnginesAgree rather than a
# distinct test.
#
# Optimised builds only, and never under a sanitizer. Two reasons, both learned
# the hard way: in debug this takes 12 minutes, and the asan-ubsan and tsan
# presets are RelWithDebInfo, so matching on build type alone ran the 5M-op
# differential under sanitizers at 10-50x slowdown until it hit the 3600s timeout.
# Re-proving the same invariant in four configurations is not worth an hour of CI.
if(CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo|Release" AND LOB_SANITIZER STREQUAL "none")
  add_test(NAME differential_long COMMAND test_differential
                                  --gtest_filter=Differential.EnginesAgree)
  set_tests_properties(differential_long PROPERTIES
                       TIMEOUT 3600
                       ENVIRONMENT "LOB_DIFF_OPS=5000000")
endif()

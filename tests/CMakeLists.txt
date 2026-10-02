# gtest_discover_tests registers each GoogleTest case as its own CTest test, so
# a failure names the case instead of "the test binary".
include(GoogleTest)

add_subdirectory(unit)
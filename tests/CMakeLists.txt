# gtest_discover_tests registers each GoogleTest case as its own CTest test, so
# a failure names the case instead of "the test binary".
include(GoogleTest)

add_subdirectory(unit)
add_subdirectory(property)
add_subdirectory(differential)
add_subdirectory(journal)
add_subdirectory(runtime)
add_subdirectory(protocol)
add_subdirectory(gateway)
add_subdirectory(e2e)

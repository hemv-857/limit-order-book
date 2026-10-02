add_executable(test_invariants invariant_probe.cpp)
target_link_libraries(test_invariants PRIVATE lob_core GTest::gtest_main lob_warnings)
gtest_discover_tests(test_invariants)

# The long randomised run is registered as its own test with a generous
# timeout so `ctest` does not kill it mid-stream on a slow machine.
add_test(NAME invariant_probe_long
         COMMAND test_invariants --gtest_filter=InvariantProbe.MixedStreamKeepsEveryInvariant)
set_tests_properties(invariant_probe_long PROPERTIES TIMEOUT 900)

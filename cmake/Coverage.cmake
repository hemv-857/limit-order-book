# Coverage is only meaningful for the deterministic libraries, so instrumentation
# is applied to the project libraries alone and the report is filtered to them.
# Otherwise every line of GoogleTest and Benchmark would count against us.

option(LOB_ENABLE_COVERAGE "Instrument lob_core and lob_journal for coverage" OFF)

if(LOB_ENABLE_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Coverage requires GCC or Clang")
  endif()
  add_compile_options(--coverage -O0 -g)
  add_link_options(--coverage)
endif()

find_program(LOB_LCOV lcov)
find_program(LOB_GCOV gcov)
find_program(LOB_GENHTML genhtml)

add_custom_target(coverage
  COMMAND ${CMAKE_COMMAND} -E echo "-- running test suite under coverage"
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  COMMAND ${CMAKE_COMMAND} -E echo "-- collecting coverage for src/core and src/journal"
  COMMAND gcov -r -b -c -o ${CMAKE_BINARY_DIR}/CMakeFiles
          ${PROJECT_SOURCE_DIR}/src/core/*.cpp
          ${PROJECT_SOURCE_DIR}/src/journal/*.cpp
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/coverage
  COMMENT "Run tests and collect line coverage (needs lcov/gcov)")

if(TARGET coverage AND LOB_LCOV)
  add_custom_target(coverage-report
    COMMAND ${LOB_LCOV} --capture --directory ${CMAKE_BINARY_DIR}/coverage
            --output-file ${CMAKE_BINARY_DIR}/coverage.info
            --rc lcov_branch_coverage=1
    COMMAND ${LOB_LCOV} --extract ${CMAKE_BINARY_DIR}/coverage.info
            "${PROJECT_SOURCE_DIR}/src/core/*" "${PROJECT_SOURCE_DIR}/src/journal/*"
            --rc lcov_branch_coverage=1 --rc genhtml_branch_coverage=1
            --output-file ${CMAKE_BINARY_DIR}/coverage-filtered.info
    COMMAND ${LOB_GENHTML} ${CMAKE_BINARY_DIR}/coverage-filtered.info
            --branch-coverage --output-directory ${CMAKE_BINARY_DIR}/coverage-html
    DEPENDS coverage
    COMMENT "Generate filtered HTML coverage report")
endif()
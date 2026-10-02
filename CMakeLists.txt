cmake_minimum_required(VERSION 3.22)
project(limit_order_book
        VERSION 1.0.0
        DESCRIPTION "Deterministic limit order book and matching engine"
        LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position-independent code everywhere: the core is linked into tools, tests,
# benchmarks and sanitizer builds, and PIE keeps ASan usable on all targets.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Put every artefact in one predictable place so the presets and the CI agree.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(LOB_BUILD_TESTS    "Build the test suite"          ON)
option(LOB_BUILD_BENCHES   "Build Google Benchmark suite"  ON)
option(LOB_BUILD_FUZZERS   "Build fuzz targets"            ON)
option(LOB_BUILD_TOOLS     "Build venue and CLI tools"     ON)
option(LOB_WERROR          "Treat warnings as errors"      ON)

include(Warnings)
include(Sanitizers)
include(Coverage)
include(Dependencies)

add_subdirectory(src)

if(LOB_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(LOB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LOB_BUILD_BENCHES)
  add_subdirectory(bench)
endif()

if(LOB_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

message(STATUS "")
message(STATUS "=== limit_order_book ${PROJECT_VERSION} ===")
message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler       : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  sanitizers     : ${LOB_SANITIZERS}")
message(STATUS "  werror         : ${LOB_WERROR}")
message(STATUS "  tests/bench/fuzz: ${LOB_BUILD_TESTS}/${LOB_BUILD_BENCHES}/${LOB_BUILD_FUZZERS}")
message(STATUS "  libFuzzer      : ${LOB_HAVE_LIBFUZZER}")
message(STATUS "")
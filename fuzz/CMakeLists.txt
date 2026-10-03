# Fuzz targets are plain functions in targets.cpp, with no dependency on any
# fuzzing runtime. libFuzzer is linked only where its runtime exists; everywhere
# else the portable driver in tests/fuzz drives the same code.
add_library(lob_fuzz_targets STATIC targets.cpp)
target_include_directories(lob_fuzz_targets PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lob_fuzz_targets PUBLIC lob_protocol lob_journal)

# Coverage-guided drivers, built only when the toolchain can link libFuzzer. They
# are the better driver where available; the portable one is what always runs.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles("int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long){return 0;}"
                          LOB_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

if(LOB_HAVE_LIBFUZZER)
  foreach(target codec journal engine)
    add_executable(fuzz_${target} fuzz_${target}.cc)
    target_link_libraries(fuzz_${target} PRIVATE lob_fuzz_targets)
    target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
  endforeach()
  message(STATUS "libFuzzer available: coverage-driven targets enabled")
else()
  message(STATUS "libFuzzer runtime not available on this toolchain; "
                 "the portable corpus+mutation driver is the one that runs")
endif()

# Fuzz targets are plain functions in targets.cpp, with no dependency on any
# fuzzing runtime. libFuzzer is linked only where its runtime exists; everywhere
# else the portable driver in tests/fuzz drives the same code.
add_library(lob_fuzz_targets STATIC targets.cpp)
target_include_directories(lob_fuzz_targets PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lob_fuzz_targets PUBLIC lob_protocol lob_journal)

# Coverage-guided drivers, built only when the toolchain can link libFuzzer. They
# are the better driver where available; the portable one is what always runs.
# Two ways to get the coverage-guided targets:
#   -LOB_FUZZER_TARGETS=ON  build them, and let the linker decide. This is what CI
#                           uses, because a missing libFuzzer runtime then fails
#                           with the actual linker error instead of silently
#                           skipping the targets and leaving "unknown target".
#   -otherwise              probe, and fall back to the portable driver.
# The probe only ever reports "Failed", which is why the CI leg spent several
# rounds guessing at the cause.
include(CheckCXXSourceCompiles)
option(LOB_FUZZER_TARGETS "Build the coverage-guided libFuzzer targets" OFF)
set(LOB_HAVE_LIBFUZZER ${LOB_FUZZER_TARGETS})
if(NOT LOB_FUZZER_TARGETS)
  # -fsanitize=fuzzer has to be a required *link* option as well as a compile
  # flag; with it only on the compile line the probe links no fuzzer runtime and
  # fails on the missing entry point.
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
  check_cxx_source_compiles("int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long){return 0;}"
                            LOB_HAVE_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
endif()

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

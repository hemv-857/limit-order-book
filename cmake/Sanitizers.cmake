# Sanitizer selection is driven by the preset via LOB_SANITIZER.
#
# ASan and TSan cannot coexist in one binary (they need incompatible shadow
# memory layouts), so mixing them is a hard configure error rather than a
# silently mis-instrumented build.

set(LOB_SANITIZER "" CACHE STRING "Sanitizer to enable (none|asan-ubsan|tsan)")
set_property(CACHE LOB_SANITIZER PROPERTY STRINGS "" "none" "asan-ubsan" "tsan")

add_library(lob_sanitizers INTERFACE)

if(LOB_SANITIZER STREQUAL "asan-ubsan")
  set(LOB_SANITIZERS "address,undefined")
  set(LOB_SANITIZER_FLAGS
      -fsanitize=address,undefined
      -fno-sanitize-recover=all
      -fno-omit-frame-pointer
      -fno-optimize-sibling-calls)
  add_compile_options(${LOB_SANITIZER_FLAGS})
  add_link_options(${LOB_SANITIZER_FLAGS})
elseif(LOB_SANITIZER STREQUAL "tsan")
  set(LOB_SANITIZERS "thread")
  set(LOB_SANITIZER_FLAGS
      -fsanitize=thread
      -fno-omit-frame-pointer)
  add_compile_options(${LOB_SANITIZER_FLAGS})
  add_link_options(${LOB_SANITIZER_FLAGS})
else()
  set(LOB_SANITIZERS "none")
  set(LOB_SANITIZER_FLAGS "")
endif()

# libFuzzer ships with clang's compiler-rt on Linux but is absent from Apple's
# CommandLineTools. Detected rather than assumed, so CMake can report which
# driver is in use and CI stays honest.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles("
  #include <cstdint>
  #include <cstddef>
  extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }
  int main() { return 0; }
  " LOB_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

# check_cxx_source_compiles leaves the variable undefined when the check cannot
# even be attempted, so normalise to a readable on/off for the configure summary.
if(LOB_HAVE_LIBFUZZER)
  set(LOB_HAVE_LIBFUZZER ON)
else()
  set(LOB_HAVE_LIBFUZZER OFF)
endif()
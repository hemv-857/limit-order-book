# Third-party dependencies are fetched once, pinned to exact tags, and cached in
# the build tree. Nothing here is allowed to leak into lob_core: the core links
# the standard library only.

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

if(LOB_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if(LOB_BUILD_BENCHES)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE)
  set(BENCHMARK_ENABLE_TESTING   OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR    OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()

# Upstream projects ship their own strict warning flags, including -Werror, and
# our toolchain is often newer than the code they were written against
# (GoogleTest 1.15 trips Apple Clang 21's -Wcharacter-conversion). Our own code
# keeps the full -Werror treatment; fetched code is silenced per target so it
# can never fail our build for a reason we cannot fix.
foreach(_dep_target gtest gtest_main benchmark benchmark_main benchmark_internal)
  if(TARGET ${_dep_target})
    target_compile_options(${_dep_target} PRIVATE -w)
  endif()
endforeach()

# One helper so every in-tree target gets the same include roots and warnings
# without repeating boilerplate. `LIB` may be omitted for interface targets.
function(lob_add_library name)
  cmake_parse_arguments(ARG "INTERFACE" "LIB" "SOURCES;DEPENDS" ${ARGN})
  if(ARG_INTERFACE)
    add_library(${name} INTERFACE)
  else()
    add_library(${name} STATIC ${ARG_SOURCES})
    target_include_directories(${name} PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${name} PRIVATE lob_warnings)
    if(ARG_DEPENDS)
      target_link_libraries(${name} PUBLIC ${ARG_DEPENDS})
    endif()
  endif()
endfunction()
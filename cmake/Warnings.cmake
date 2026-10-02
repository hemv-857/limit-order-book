# Strict warning set, applied to first-party targets only.
#
# -Wconversion is the expensive one: it forces every integer narrowing in the
# price/quantity arithmetic to be an explicit cast, which is exactly where
# silent tick/lot bugs would otherwise hide.

add_library(lob_warnings INTERFACE)

if(MSVC)
  target_compile_options(lob_warnings INTERFACE /W4 /permissive- /Zc:preprocessor)
  if(LOB_WERROR)
    target_compile_options(lob_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(lob_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
  )
  if(LOB_WERROR)
    target_compile_options(lob_warnings INTERFACE -Werror)
  endif()
endif()

# Keep debug information even in release: perf-style profiling and the crash
# tests both need line info, and it costs nothing at runtime.
add_compile_options($<$<CONFIG:Debug>:-g3>)

# Deterministic floating point is irrelevant to the core (it has none), but the
# tools print measurements; forbid fast-math contraction so printed numbers mean
# what they say.
if(NOT MSVC)
  add_compile_options(-ffp-contract=off)
endif()
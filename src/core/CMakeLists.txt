# src/core: the deterministic matching engine.
#
# Links nothing but the standard library. No sockets, no clock, no filesystem, no
# threads, no exceptions on the hot path. Everything external is injected.

lob_add_library(lob_core
  SOURCES
    types.cpp
)

target_include_directories(lob_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
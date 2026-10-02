add_library(lob_util STATIC crc32c.cpp)
target_include_directories(lob_util PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(lob_protocol STATIC codec.cpp)
target_include_directories(lob_protocol PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lob_protocol PUBLIC lob_core lob_util)

add_library(lob_venue STATIC venue.cpp)
target_include_directories(lob_venue PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lob_venue PUBLIC lob_gateway lob_runtime)

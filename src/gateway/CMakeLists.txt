add_library(lob_gateway STATIC market_data.cpp)
target_include_directories(lob_gateway PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lob_gateway PUBLIC lob_protocol)

add_library(lob_journal STATIC journal.cpp)
target_include_directories(lob_journal PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lob_journal PUBLIC lob_core lob_util)

add_executable(venue venue_main.cpp)
target_link_libraries(venue PRIVATE lob_venue lob_warnings)

add_executable(lobctl lobctl.cpp)
target_link_libraries(lobctl PRIVATE lob_protocol lob_warnings)

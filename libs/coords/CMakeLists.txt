add_library(x4sb_coords STATIC src/coords.cpp)
add_library(x4sb::coords ALIAS x4sb_coords)
target_include_directories(x4sb_coords PUBLIC include)
target_link_libraries(x4sb_coords PUBLIC x4sb_data)

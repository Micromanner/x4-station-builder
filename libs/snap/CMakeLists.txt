add_library(x4sb_snap STATIC src/snap.cpp)
add_library(x4sb::snap ALIAS x4sb_snap)
target_include_directories(x4sb_snap PUBLIC include)
target_link_libraries(x4sb_snap PUBLIC x4sb_data x4sb_document)

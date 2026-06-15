add_library(x4sb_archive STATIC src/archive.cpp)
add_library(x4sb::archive ALIAS x4sb_archive)
target_include_directories(x4sb_archive PUBLIC include)

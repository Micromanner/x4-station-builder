add_library(x4sb_platform STATIC src/platform.cpp)
add_library(x4sb::platform ALIAS x4sb_platform)
target_include_directories(x4sb_platform PUBLIC include)
# Uses only the C++17 standard library (<filesystem>, <cstdlib>); no other deps.

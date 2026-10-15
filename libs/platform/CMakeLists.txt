add_library(x4sb_platform STATIC src/platform.cpp)
add_library(x4sb::platform ALIAS x4sb_platform)
target_include_directories(x4sb_platform PUBLIC include)
# std <filesystem>/<cstdlib> + vendored nlohmann-json (settings file).
target_link_libraries(x4sb_platform PRIVATE nlohmann_json::nlohmann_json)

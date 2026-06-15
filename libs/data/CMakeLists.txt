add_library(x4sb_data STATIC src/catalog.cpp)
add_library(x4sb::data ALIAS x4sb_data)
target_include_directories(x4sb_data PUBLIC include)
target_link_libraries(x4sb_data PUBLIC nlohmann_json::nlohmann_json)

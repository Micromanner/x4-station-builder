add_library(x4sb_planio STATIC src/plan.cpp)
add_library(x4sb::planio ALIAS x4sb_planio)
target_include_directories(x4sb_planio PUBLIC include)
target_link_libraries(x4sb_planio PUBLIC x4sb_data x4sb_document x4sb_coords pugixml::pugixml)

add_library(x4sb_autolayout STATIC src/autolayout.cpp)
add_library(x4sb::autolayout ALIAS x4sb_autolayout)
target_include_directories(x4sb_autolayout PUBLIC include)
target_link_libraries(x4sb_autolayout PUBLIC x4sb_data x4sb_document x4sb_snap)

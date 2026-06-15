add_library(x4sb_document STATIC src/station.cpp)
add_library(x4sb::document ALIAS x4sb_document)
target_include_directories(x4sb_document PUBLIC include)
target_link_libraries(x4sb_document PUBLIC x4sb_data)

add_library(x4sb_editorcore STATIC src/editor_state.cpp src/gizmo.cpp src/mesh_paths.cpp)
add_library(x4sb::editorcore ALIAS x4sb_editorcore)
target_include_directories(x4sb_editorcore PUBLIC include)
target_link_libraries(x4sb_editorcore PUBLIC x4sb_data x4sb_document x4sb_snap)

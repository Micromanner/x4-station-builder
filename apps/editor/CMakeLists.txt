# Editor app — the only target that links raylib. raygui ships as a header, so a
# dedicated C TU (raygui_impl.c) provides its implementation.
add_executable(x4sb-editor
  src/main.cpp
  src/app_paths.cpp
  src/orbit_camera.cpp
  src/render.cpp
  src/input.cpp
  src/mesh_cache.cpp
  src/snaptest.cpp
  src/raygui_impl.c)

# SYSTEM so the first-party -Werror harness ignores warnings from the raygui header.
target_include_directories(x4sb-editor SYSTEM PRIVATE ${RAYGUI_INCLUDE_DIR})

target_link_libraries(x4sb-editor PRIVATE
  x4sb_data
  x4sb_document
  x4sb_snap
  x4sb_editorcore
  x4sb_autolayout
  x4sb_planio
  x4sb_coords
  x4sb_platform
  raylib)

# On Windows, building the GUI subsystem avoids a console window in Release.
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set_target_properties(x4sb-editor PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

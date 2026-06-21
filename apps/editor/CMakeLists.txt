# Editor app — the only target that links raylib. raygui ships as a header, so a
# dedicated C TU (raygui_impl.c) provides its implementation.
add_executable(x4sb-editor
  src/main.cpp
  src/app_paths.cpp
  src/ui_fonts.cpp
  src/orbit_camera.cpp
  src/render.cpp
  src/input.cpp
  src/mesh_cache.cpp
  src/mesh_load.cpp
  src/snaptest.cpp
  src/megashot.cpp
  src/gizmoshot.cpp
  src/profile.cpp
  src/iprofile.cpp
  src/connbench.cpp
  src/loadbench.cpp
  src/rdc_api.cpp
  src/rdc_capture.cpp
  src/plan_io.cpp
  src/raygui_impl.c
  src/clay_impl.c
  src/clay_raylib.cpp
  src/ui_topbar.cpp
  src/ui_palette.cpp
  src/ui_inspector.cpp
  src/uishot.cpp)

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
  raylib
  clay::clay
  renderdoc::app)

# Clay requires C++20 (clay.h #errors below it). The editor is the only target
# that includes Clay; the render-free libs remain C++17.
set_target_properties(x4sb-editor PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Tracy profiler client (full-tracy preset only). Linking it defines TRACY_ENABLE
# PUBLIC, switching src/profiling.hpp onto the real zone macros.
if(X4SB_TRACY)
  target_link_libraries(x4sb-editor PRIVATE tracy::client)
endif()

# On Windows, building the GUI subsystem avoids a console window in Release.
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set_target_properties(x4sb-editor PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

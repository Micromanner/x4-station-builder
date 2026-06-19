add_executable(x4sb_tests
  doctest_main.cpp
  math_test.cpp
  coords_test.cpp
  document_test.cpp
  snap_test.cpp
  connector_grid_test.cpp
  pick_test.cpp
  display_flip_test.cpp
  display_name_test.cpp
  editorcore_test.cpp
  mesh_paths_test.cpp
  gizmo_test.cpp
  gizmo_screen_size_test.cpp
  archive_test.cpp
  assetpipe_test.cpp
  clearance_test.cpp
  xmf_test.cpp
  plan_test.cpp
  catalog_test.cpp
  platform_test.cpp
  moduleindex_test.cpp
  wares_test.cpp
  localization_test.cpp
  catalogbuild_test.cpp
  meshconvert_test.cpp
  camera_math_test.cpp)

target_link_libraries(x4sb_tests PRIVATE
  doctest::doctest
  nlohmann_json::nlohmann_json
  x4sb_archive
  x4sb_assetpipe
  x4sb_data
  x4sb_document
  x4sb_snap
  x4sb_editorcore
  x4sb_autolayout
  x4sb_planio
  x4sb_coords
  x4sb_platform)

target_include_directories(x4sb_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/editor/src)

add_test(NAME x4sb_tests COMMAND x4sb_tests)

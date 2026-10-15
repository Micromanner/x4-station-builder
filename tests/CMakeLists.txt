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
  clusterbg_test.cpp
  lensfx_test.cpp
  sectorindex_test.cpp
  clearance_test.cpp
  uv_animation_test.cpp
  local_lights_test.cpp
  station_gpu_test.cpp
  xmf_test.cpp
  dds_test.cpp
  plan_test.cpp
  catalog_test.cpp
  platform_test.cpp
  moduleindex_test.cpp
  wares_test.cpp
  localization_test.cpp
  catalogbuild_test.cpp
  material_library_test.cpp
  metadata_cache_test.cpp
  camera_math_test.cpp
  autolayout_test.cpp
  autolayout_roles_test.cpp
  autolayout_connectors_test.cpp
  autolayout_peripheral_test.cpp
  autolayout_metrics_test.cpp
  autolayout_search_test.cpp
  autolayout_anneal_test.cpp
  autolayout_objective_test.cpp
  autolayout_moves_test.cpp
  autolayout_serialize_test.cpp
  autolayout_seed_test.cpp
  background_manifest_test.cpp
  nebula_fog_test.cpp)

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

if(X4SB_BUILD_EDITOR)
  add_executable(x4sb_dds_gpu_test dds_gpu_test.cpp xmf_gpu_test.cpp
    ../apps/editor/src/dds_texture.cpp ../apps/editor/src/texture_mips.cpp
    ../apps/editor/src/xmf_model.cpp)
  target_include_directories(x4sb_dds_gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/apps/editor/src)
  target_link_libraries(x4sb_dds_gpu_test PRIVATE x4sb_assetpipe raylib)
  add_test(NAME x4sb_dds_gpu_test COMMAND x4sb_dds_gpu_test)
  set_tests_properties(x4sb_dds_gpu_test PROPERTIES LABELS gpu)
endif()

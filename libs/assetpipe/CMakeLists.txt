# Shared asset-pipeline logic (spec §4A): archive + module XML + mesh conversion.
# Render-free; pipeline.exe is a thin CLI over it.
add_library(x4sb_assetpipe STATIC
  src/catalogbuild.cpp
  src/meshconvert.cpp
  src/component.cpp
  src/localization.cpp
  src/moduleindex.cpp
  src/wares.cpp
  src/xmf.cpp
  src/gltf.cpp)
add_library(x4sb::assetpipe ALIAS x4sb_assetpipe)
target_include_directories(x4sb_assetpipe PUBLIC include)
target_link_libraries(x4sb_assetpipe
  PUBLIC x4sb_data
  PRIVATE x4sb_archive pugixml::pugixml nlohmann_json::nlohmann_json sinfl::sinfl)

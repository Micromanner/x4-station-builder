# Asset pipeline CLI (spec §4A). Separate executable, but the editor launches it
# as a child process behind a first-run wizard — end users never touch a shell.
# Thin wrapper over libs/assetpipe (archive read + module XML + .xmf -> glTF).
add_executable(x4sb-pipeline src/main.cpp)
target_link_libraries(x4sb-pipeline PRIVATE
  x4sb_assetpipe
  x4sb_archive
  x4sb_data
  x4sb_coords
  x4sb_platform)

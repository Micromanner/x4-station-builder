# The pipeline CLI is render-free and always builds. The editor needs raylib and
# is gated behind X4SB_BUILD_EDITOR.
add_subdirectory(pipeline)

if(X4SB_BUILD_EDITOR)
  add_subdirectory(editor)
endif()

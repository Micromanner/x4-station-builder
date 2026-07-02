# The pipeline CLI and the layout diagnostic are render-free and always build. The
# editor needs raylib and is gated behind X4SB_BUILD_EDITOR.
add_subdirectory(pipeline)
add_subdirectory(layoutlab)

if(X4SB_BUILD_EDITOR)
  add_subdirectory(editor)
endif()

# Auto-layout diagnostic CLI (render-free). Runs the layout engine on a real catalog and
# prints layoutReport()'s topology dump so seed/anneal output can be read, not eyeballed.
add_executable(x4sb-layoutlab src/main.cpp)
target_link_libraries(x4sb-layoutlab PRIVATE
  x4sb_autolayout
  x4sb_data
  x4sb_document
  x4sb_planio)

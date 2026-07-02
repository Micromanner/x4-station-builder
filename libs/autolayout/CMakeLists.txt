add_library(x4sb_autolayout STATIC
  src/autolayout.cpp
  src/roles.cpp
  src/connectors.cpp
  src/peripheral.cpp
  src/metrics.cpp
  src/search.cpp
  src/anneal.cpp
  src/objective.cpp
  src/moves.cpp
  src/serialize.cpp
  src/report.cpp
  src/seed.cpp)
add_library(x4sb::autolayout ALIAS x4sb_autolayout)
target_include_directories(x4sb_autolayout PUBLIC include)
target_link_libraries(x4sb_autolayout PUBLIC x4sb_data x4sb_document x4sb_snap)

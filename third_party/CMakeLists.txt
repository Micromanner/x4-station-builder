# Vendored third-party dependencies — frozen copies committed to the repo so no
# future CMake/compiler/network change can break the build. We compile them with
# OUR OWN targets and never invoke their build systems, which permanently avoids
# the CMake-version coupling that FetchContent leaves exposed. See README.md for
# exact versions, sources, and how to update.

# Includes are marked SYSTEM so the first-party -Werror harness (cmake/warnings.cmake)
# never trips on warnings originating inside these vendored headers.

# doctest v2.5.2 — single-header test framework.
add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest)

# nlohmann/json v3.12.0 — single-header JSON (the catalog.json cache).
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/json)

# pugixml v1.15 — single-source XML parser, compiled here as a static lib.
add_library(pugixml STATIC ${CMAKE_CURRENT_SOURCE_DIR}/pugixml/pugixml.cpp)
add_library(pugixml::pugixml ALIAS pugixml)
target_include_directories(pugixml SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pugixml)

# raygui 4.0 — single-header immediate-mode GUI. The implementation TU lives in
# apps/editor (raygui_impl.c); here we only expose the header directory.
set(RAYGUI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/raygui"
    CACHE INTERNAL "Vendored raygui header directory")

# sinfl — single-header DEFLATE/zlib INFLATE (MIT / public domain). The asset
# pipeline needs it to decompress X4 .xmf mesh buffers (their vertex/index data
# is zlib-wrapped) and libs/ may not link raylib's bundled zlib. Compiled as its
# own C TU here so its implementation stays outside the first-party -Werror gate.
add_library(sinfl STATIC ${CMAKE_CURRENT_SOURCE_DIR}/sinfl/sinfl_impl.c)
add_library(sinfl::sinfl ALIAS sinfl)
target_include_directories(sinfl SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sinfl)

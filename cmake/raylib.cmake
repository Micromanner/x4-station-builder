# raylib — the ONE remaining external built dependency (the renderer). It is not
# sensibly vendorable (a large C library with platform window/GL backends), so it
# stays on FetchContent, pinned to an exact tag. Everything else lives, frozen,
# under third_party/.
#
# Note: raylib tags have NO leading 'v' (use 5.5, 6.0). 5.5 is chosen for
# stability (battle-tested); 6.0 (Apr 2026) is available if newer features are
# wanted — bump GIT_TAG and pair with a matching raygui header.
include(FetchContent)

# raylib's pinned tag still declares an older cmake_minimum_required than CMake
# 4.x accepts; scope the policy shim to this fetch only.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(BUILD_GAMES    OFF CACHE INTERNAL "")
FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG        5.5
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(raylib)

# Mark raylib's headers SYSTEM for consumers so the first-party -Werror harness
# (cmake/warnings.cmake) never trips on warnings from inside raylib.h.
get_target_property(_raylib_inc raylib INTERFACE_INCLUDE_DIRECTORIES)
if(_raylib_inc)
  set_target_properties(raylib PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_raylib_inc}")
endif()

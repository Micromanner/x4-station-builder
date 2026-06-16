cmake_minimum_required(VERSION 3.20)

project(x4-station-builder
  VERSION 0.1.0
  DESCRIPTION "Standalone visual station builder for X4: Foundations"
  LANGUAGES C CXX)

# ── Language standard (decided: C++17) ──────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Verification harness: warning/sanitizer INTERFACE targets + their options
# (X4SB_WERROR, X4SB_SANITIZE). Defined here so the MinGW -static guard below can
# react to X4SB_SANITIZE; applied to first-party targets further down.
include(cmake/warnings.cmake)

# Single-config default to Debug for local dev; multi-config (VS) untouched.
get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Predictable output locations.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MinGW: statically link the GCC runtime so executables are self-contained and
# do not depend on libstdc++/libgcc/libwinpthread being resolved from PATH at
# runtime (also what we want for a distributable Windows tool). Skipped when
# sanitizing — ASan/UBSan runtimes cannot be statically linked under MinGW.
if(MINGW AND NOT X4SB_SANITIZE)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# ── Options ─────────────────────────────────────────────────────────────────
# The editor pulls in raylib (a large FetchContent build). Turn it OFF to build
# only the render-free libs, the pipeline CLI, and the tests.
option(X4SB_BUILD_EDITOR "Build the raylib/raygui editor app" ON)
option(X4SB_BUILD_TESTS  "Build unit tests (doctest)"          ON)
# Tracy frame profiler: compiles + links the vendored client into the editor and
# defines TRACY_ENABLE. OFF by default so normal builds carry zero profiling cost
# (the instrumentation macros no-op); the `full-tracy` preset turns it ON.
option(X4SB_TRACY        "Build the editor with the Tracy profiler client" OFF)

# ── Dependencies ────────────────────────────────────────────────────────────
# Vendored, frozen deps (doctest, nlohmann/json, pugixml, raygui) — compiled by
# our own targets, never via their build systems. See third_party/README.md.
add_subdirectory(third_party)
# The one external built dependency: raylib (renderer), editor builds only.
if(X4SB_BUILD_EDITOR)
  include(cmake/raylib.cmake)
endif()

# ── First-party verification harness ─────────────────────────────────────────
# Everything below is created AFTER third_party/raylib, so these gates touch only
# our code. clang-tidy is opt-in (needs LLVM installed) and no-ops if absent.
option(X4SB_CLANG_TIDY "Run clang-tidy on first-party targets during the build" OFF)
if(X4SB_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy)
  if(CLANG_TIDY_EXE)
    # extra-args, both needed for a MinGW project:
    #  • -Wno-unknown-warning-option: clang's frontend rejects the GCC-only warning
    #    flags in the compile DB (-Wduplicated-cond/-Wlogical-op/...).
    #  • --target=...-mingw32: otherwise clang defaults to the MSVC target and parses
    #    our code against Visual Studio's STL, derailing on libstdc++ constructs.
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}" "--extra-arg=-Wno-unknown-warning-option")
    if(MINGW)
      list(APPEND CMAKE_CXX_CLANG_TIDY "--extra-arg=--target=x86_64-w64-mingw32")
    endif()
    message(STATUS "x4sb: clang-tidy gate ON (${CLANG_TIDY_EXE})")
  else()
    message(WARNING "x4sb: X4SB_CLANG_TIDY=ON but clang-tidy not found — gate disabled")
  endif()
endif()

# Strict warnings (-> -Werror) and any sanitizers, linked into every target
# defined from here on (libs, apps, tests) but NOT the vendored deps above.
link_libraries(x4sb::warnings x4sb::sanitizers)

# ── Project units ───────────────────────────────────────────────────────────
add_subdirectory(libs)   # pure logic; never links raylib
add_subdirectory(apps)   # editor (raylib) + pipeline (CLI)

if(X4SB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "x4-station-builder ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD} | "
               "editor=${X4SB_BUILD_EDITOR} tests=${X4SB_BUILD_TESTS}")

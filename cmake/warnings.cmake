# Verification harness: warning + sanitizer flags for FIRST-PARTY code only.
#
# These INTERFACE targets are pulled into every first-party target via
# link_libraries() in the root CMakeLists — placed AFTER third_party/ and raylib
# are configured, so vendored deps are never compiled with our strict flags.
# All flags are scoped to $<COMPILE_LANGUAGE:CXX> so the raygui C TU is exempt.
#
# Rationale lives in CLAUDE.md ("C++ quality bar"): LLM-written C++ compiles but
# is frequently unclean/unsafe, so -Werror + sanitizers are the automated gate.

option(X4SB_WERROR "Treat first-party warnings as errors" ON)
set(X4SB_SANITIZE "" CACHE STRING
    "Sanitizers for first-party code, comma-separated, e.g. 'address,undefined'. \
Empty = off. NOTE: best run under Linux/Clang CI; MinGW support is partial and \
conflicts with -static (the root drops -static automatically when this is set).")

# ── Warnings ──────────────────────────────────────────────────────────────────
add_library(x4sb_warnings INTERFACE)
add_library(x4sb::warnings ALIAS x4sb_warnings)

# Portable across GCC and Clang.
set(_x4sb_common_warnings
  -Wall -Wextra -Wpedantic
  -Wshadow               # a declaration shadows an outer one
  -Wnon-virtual-dtor     # polymorphic base type with a non-virtual destructor
  -Wold-style-cast       # C-style cast -> use static_cast/etc.
  -Wcast-align           # cast increases required alignment
  -Wunused               # anything declared and unused
  -Woverloaded-virtual   # a derived method hides a base virtual
  -Wconversion           # implicit conversions that may alter a value (CWE-190/197)
  -Wsign-conversion      # implicit signed<->unsigned conversions
  -Wnull-dereference     # a null dereference is detected
  -Wdouble-promotion     # silent float -> double promotion
  -Wformat=2             # printf/scanf format-string checking
  -Wimplicit-fallthrough # switch fallthrough without [[fallthrough]]
)

# GCC-only extras (the project's primary toolchain is MinGW GCC 13).
# NB: -Wuseless-cast is deliberately excluded — it flags portable defensive casts
# as useless on platforms where two integer typedefs happen to coincide (e.g.
# uint64_t == unsigned long long here but not on LP64 Linux), which conflicts with
# the defensive casting -Wconversion encourages.
set(_x4sb_gcc_warnings
  -Wmisleading-indentation
  -Wduplicated-cond
  -Wduplicated-branches
  -Wlogical-op
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_x4sb_warnings ${_x4sb_common_warnings} ${_x4sb_gcc_warnings})
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_x4sb_warnings ${_x4sb_common_warnings})
elseif(MSVC)
  set(_x4sb_warnings /W4 /permissive-)
endif()

target_compile_options(x4sb_warnings INTERFACE
  "$<$<COMPILE_LANGUAGE:CXX>:${_x4sb_warnings}>")

if(X4SB_WERROR)
  if(MSVC)
    set(_x4sb_werror /WX)
  else()
    set(_x4sb_werror -Werror)
  endif()
  target_compile_options(x4sb_warnings INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:${_x4sb_werror}>")
endif()

# ── Sanitizers ────────────────────────────────────────────────────────────────
add_library(x4sb_sanitizers INTERFACE)
add_library(x4sb::sanitizers ALIAS x4sb_sanitizers)

if(X4SB_SANITIZE)
  if(MSVC)
    target_compile_options(x4sb_sanitizers INTERFACE /fsanitize=${X4SB_SANITIZE} /Zi)
  else()
    target_compile_options(x4sb_sanitizers INTERFACE
      -fsanitize=${X4SB_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(x4sb_sanitizers INTERFACE -fsanitize=${X4SB_SANITIZE})
  endif()
  message(STATUS "x4sb: sanitizers ON (${X4SB_SANITIZE})")
endif()

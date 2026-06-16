#pragma once
// Single include point for the Tracy zone macros used by the editor's render path.
//
// Default build (X4SB_TRACY=OFF): TRACY_ENABLE is undefined, the vendored Tracy
// headers are NOT on the include path, so we define the handful of macros we use
// to nothing. The instrumented call sites then cost zero and compile clean under
// the -Werror gate without any Tracy dependency.
//
// full-tracy build (X4SB_TRACY=ON): third_party `tracy` is linked, which defines
// TRACY_ENABLE PUBLIC and adds public/ to the include path, so we pull in the real
// Tracy.hpp and the macros become live instrumentation streamed to the capture CLI.
#if defined(TRACY_ENABLE)
#include "tracy/Tracy.hpp"
#else
#define ZoneScoped
#define ZoneScopedN(name)
#define FrameMark
#define FrameMarkNamed(name)
#endif

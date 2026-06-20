// Clay's single-header implementation lives in exactly one TU (see clay.h docs).
// Kept as its own C file so the editor's first-party -Werror gate never sees
// Clay's internals (the include dir is marked SYSTEM in third_party/CMakeLists.txt).
#define CLAY_IMPLEMENTATION
#include "clay.h"

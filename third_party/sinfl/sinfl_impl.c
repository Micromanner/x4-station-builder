// Single translation unit that compiles the sinfl DEFLATE/zlib inflate
// implementation. Kept as its own C TU (compiled by the vendored `sinfl` target,
// outside the first-party -Werror harness) so its internal warnings never gate
// our build. First-party code includes sinfl.h in header-only mode.
#define SINFL_IMPLEMENTATION
#include "sinfl.h"

#pragma once
// The single X4<->display coordinate choke point for the editor (parent §8).
// The libs work in X4 native space (left-handed, Y-up); raylib renders
// right-handed. The renderer flips the WHOLE scene once via scale(1,1,-1), so
// individual geometry is drawn in native X4 coords. This same self-inverse flip
// brings raylib's mouse ray (display space) back into X4 space for pick/snap.
// Pure x4sb math; never includes raylib, so it tests under the `core` preset.
#include "x4sb/data/math.hpp"

namespace x4sb {

// Display<->X4 handedness flip: negate Z. Self-inverse. Applies identically to
// points and direction vectors.
[[nodiscard]] inline Vec3 flipZ(Vec3 v) { return {v.x, v.y, -v.z}; }

}  // namespace x4sb

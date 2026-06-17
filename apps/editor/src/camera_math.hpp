#pragma once
// Pure, render-free camera geometry used by OrbitCamera. Kept header-only and
// dependent only on x4sb/data/math.hpp so it unit-tests in the headless `core`
// preset; the raylib glue in orbit_camera.cpp converts to/from ::Vector3.
#include "x4sb/data/math.hpp"

#include <algorithm>
#include <cmath>

namespace x4sb::editor {

[[nodiscard]] inline Vec3 normalized(Vec3 v) {
  const double len = length(v);
  return len < 1e-12 ? Vec3{0, 0, 0} : v * (1.0 / len);
}

// Orthonormal camera basis derived from the eye->target forward direction.
struct CameraBasis {
  Vec3 right;
  Vec3 up;
  Vec3 forward;
};

[[nodiscard]] inline CameraBasis cameraBasis(Vec3 forward, Vec3 worldUp) {
  const Vec3 f = normalized(forward);
  // X4 uses a left-handed coordinate system; cross(worldUp, f) gives the LH
  // "right" vector ({-1,0,0} for forward=-Z, worldUp=+Y) rather than the RH +X.
  const Vec3 r = normalized(cross(worldUp, f));
  return {r, cross(f, r), f};  // f,r orthonormal => cross(f,r) is already unit
}

// World offset to add to the orbit target for a mouse delta of (dx, dy) pixels.
// `scale` converts pixels->world at the pivot depth (caller supplies it as
// 2 * distance * tan(fovy/2) / viewportHeightPx). Both axes pan the camera the
// way the cursor moves: drag right (dx>0) slides the pivot -right, drag down
// (dy>0) slides it -up — so the view tracks the drag, not the inverse.
[[nodiscard]] inline Vec3 panOffset(const CameraBasis& b, double dx, double dy, double scale) {
  return b.right * (-dx * scale) + b.up * (-dy * scale);
}

// WASD-style bulk fly: world-space offset to translate the whole rig (both pivot
// and eye, so look direction and orbit distance are preserved) for one frame.
// `forward`/`strafe`/`rise` are the net key inputs in {-1,0,1} (W-S, D-A, E-C);
// forward follows the look direction (basis.forward), rise is along world up.
// The direction is normalized so diagonals aren't faster, and zero input yields
// zero motion. `speed` is world units this frame (caller scales by distance + dt).
[[nodiscard]] inline Vec3 flyOffset(const CameraBasis& b, Vec3 worldUp, double forward,
                                    double strafe, double rise, double speed) {
  const Vec3 dir = b.forward * forward + b.right * strafe + normalized(worldUp) * rise;
  return normalized(dir) * speed;
}

struct ZoomResult {
  Vec3 target;
  double distance;
};

// Dolly toward the point under the cursor. `k` is the raw zoom factor (k<1 zooms
// in). The pivot migrates toward the focal point by the *effective* factor
// (newDistance/distance) so the focal point stays put even when the distance
// clamp bites. If the cursor ray is near-parallel to the view plane (grazing),
// falls back to a plain dolly with the pivot unchanged, so an edge-of-screen
// scroll cannot fling the pivot to infinity.
[[nodiscard]] inline ZoomResult zoomTowardCursor(Vec3 target, double distance, Vec3 forward,
                                                 Vec3 rayOrigin, Vec3 rayDir, double k,
                                                 double minDistance, double maxDistance) {
  const double newDistance = std::clamp(distance * k, minDistance, maxDistance);
  const double effK = distance > 0.0 ? newDistance / distance : 1.0;
  const Vec3 f = normalized(forward);
  const Vec3 dir = normalized(rayDir);
  const double denom = dot(dir, f);
  if (std::abs(denom) < 1e-4) return {target, newDistance};  // grazing: plain dolly
  const double t = dot(target - rayOrigin, f) / denom;
  const Vec3 focal = rayOrigin + dir * t;
  return {focal + (target - focal) * effK, newDistance};
}

}  // namespace x4sb::editor

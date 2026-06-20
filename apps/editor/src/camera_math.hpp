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

// World units spanned per screen pixel at `distance` from the eye, for vertical
// field of view `fovyRad` and viewport height `heightPx`. The pan path multiplies
// a pixel drag delta by this to stay zoom-invariant; it is the inverse of
// render.cpp's projFactor (pixels per world unit).
[[nodiscard]] inline double pixelsToWorldAtDepth(double fovyRad, double heightPx, double distance) {
  return 2.0 * distance * std::tan(fovyRad * 0.5) / heightPx;
}

// World offset to add to the orbit target for a mouse delta of (dx, dy) pixels.
// `scale` converts pixels->world at the pivot depth (caller supplies it via
// pixelsToWorldAtDepth). Both axes pan the camera the way the cursor moves: drag
// right (dx>0) slides the pivot -right, drag down (dy>0) slides it -up — so the
// view tracks the drag, not the inverse.
[[nodiscard]] inline Vec3 panOffset(const CameraBasis& b, double dx, double dy, double scale) {
  return b.right * (-dx * scale) + b.up * (-dy * scale);
}

// WASD-style bulk fly: world-space offset to translate the whole rig (both pivot
// and eye, so look direction and orbit distance are preserved) for one frame.
// `forward`/`strafe`/`rise` are the net key inputs in {-1,0,1} (W-S, A-D, Z-X);
// forward follows the look direction (basis.forward), rise is along world up.
// The direction is normalized so diagonals aren't faster, and zero input yields
// zero motion. `speed` is world units this frame (caller scales by distance + dt).
[[nodiscard]] inline Vec3 flyOffset(const CameraBasis& b, Vec3 worldUp, double forward,
                                    double strafe, double rise, double speed) {
  const Vec3 dir = b.forward * forward + b.right * strafe + normalized(worldUp) * rise;
  return normalized(dir) * speed;
}

// Net camera fly + yaw inputs resolved from the keyboard, each in {-1, 0, 1}.
struct FlyInput {
  double forward;  // W - S (along the look direction)
  double strafe;   // A - D (basis.right; A points screen-left under the LH basis)
  double rise;     // Z - X (world up; Z=up, X=down)
  double yaw;      // Q - E (turn left / right)
};

// Resolve the WASD fly / ZX rise / QE yaw keys into net inputs. Holding Ctrl yields
// zero motion (known-issues 3.10): the camera must ignore every fly/yaw key while a
// Ctrl chord is active so Ctrl+S / Ctrl+O / Ctrl+Z don't also drift or turn the view.
[[nodiscard]] inline FlyInput resolveFlyInput(bool w, bool s, bool a, bool d, bool z, bool x,
                                              bool q, bool e, bool ctrl) {
  if (ctrl) return {0.0, 0.0, 0.0, 0.0};
  const auto net = [](bool pos, bool neg) { return (pos ? 1.0 : 0.0) - (neg ? 1.0 : 0.0); };
  return {net(w, s), net(a, d), net(z, x), net(q, e)};
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

// Dolly toward an explicit world `focus` point, keeping it fixed on screen: the
// pivot migrates toward it by the effective factor (newDistance/distance) so the
// clamp can't drift the focus. Unlike zoomTowardCursor this takes the focal point
// directly (the shell hit-tests the scene under the cursor), so passing focus ==
// target yields a plain dolly with the pivot unchanged — the over-empty-space case,
// which avoids the cursor-ward pivot drift the ray-plane approximation produced.
[[nodiscard]] inline ZoomResult zoomTowardPoint(Vec3 target, double distance, Vec3 focus, double k,
                                                double minDistance, double maxDistance) {
  const double newDistance = std::clamp(distance * k, minDistance, maxDistance);
  const double effK = distance > 0.0 ? newDistance / distance : 1.0;
  return {focus + (target - focus) * effK, newDistance};
}

}  // namespace x4sb::editor

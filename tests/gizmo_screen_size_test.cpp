// Regression guard for the "gizmo size scales with camera distance" bug.
// Faithfully reproduces the editor's orbit rig (orbit_camera.cpp) + the real
// gizmoScale()/gizmoScaleFor() math, then projects the gizmo's axis arms to SCREEN
// PIXELS with exact pinhole math (== raylib's perspective GetWorldToScreen up to the
// constant principal-point offset, which cancels in pixel *differences*). A
// constant-on-screen gizmo must keep the same pixel radius as the camera moves.
//
// The bug: gizmoScale used to floor the camera-space depth by 25% of the ORBIT
// distance. zoom-toward-cursor routinely parks the orbit pivot far from the selected
// part, so the floor — keyed to the zoom distance — drove the size and the handle
// tracked camera distance. The fix floors by the part's OWN Euclidean eye distance,
// so a visible handle is a constant pixel size no matter where the pivot sits.
#include "camera_math.hpp"  // x4sb::editor::{cameraBasis, zoomTowardCursor}
#include "doctest/doctest.h"
#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/gizmo.hpp"

#include <algorithm>
#include <cmath>

using namespace x4sb;
using x4sb::editor::CameraBasis;
using x4sb::editor::cameraBasis;
using x4sb::editor::ZoomResult;
using x4sb::editor::zoomTowardCursor;

namespace {

constexpr double kFovYDeg = 60.0;  // OrbitCamera::cam_.fovy
constexpr double kScreenH = 720.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kAspect = 1280.0 / 720.0;

// Eye position for the editor's orbit rig (orbit_camera.cpp rebuild()).
[[nodiscard]] Vec3 orbitEye(Vec3 target, double distance, double yaw, double pitch) {
  const double cp = std::cos(pitch);
  return Vec3{target.x + distance * cp * std::sin(yaw), target.y + distance * std::sin(pitch),
              target.z + distance * cp * std::cos(yaw)};
}

struct Px {
  double x{0}, y{0};
};

// Pinhole projection: pixel position of a world point relative to screen center.
// f = focal length in pixels = H / (2 tan(fovy/2)).
[[nodiscard]] Px project(Vec3 p, Vec3 eye, const CameraBasis& b, double f) {
  const Vec3 d = p - eye;
  const double zc = dot(d, b.forward);
  return Px{dot(d, b.right) / zc * f, dot(d, b.up) / zc * f};
}

[[nodiscard]] double pxDist(Px a, Px c) { return std::hypot(a.x - c.x, a.y - c.y); }

// On-screen size of the gizmo = its longest axis arm, in pixels.
[[nodiscard]] double gizmoPixelRadius(Vec3 origin, double scale, Vec3 eye, const CameraBasis& b,
                                      double f) {
  const Px po = project(origin, eye, b, f);
  double maxd = 0.0;
  for (const GizmoHandle h : {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ}) {
    maxd = std::max(maxd, pxDist(po, project(origin + gizmoAxisDir(h) * scale, eye, b, f)));
  }
  return maxd;
}

// gizmoScaleFor() reduced to pure math (input.cpp): floor by the part's OWN eye distance.
[[nodiscard]] double scaleFor(Vec3 module, Vec3 eye, const CameraBasis& b) {
  const double depth = dot(module - eye, b.forward);
  const double dist = length(module - eye);
  return gizmoScale(depth, dist);
}

// Module's horizontal screen NDC (|ndcX|>1 == off-screen left/right).
[[nodiscard]] double moduleNdcX(Vec3 module, Vec3 eye, const CameraBasis& b) {
  const Vec3 m = module - eye;
  return dot(m, b.right) / dot(m, b.forward) / (std::tan(kFovYDeg * kDeg2Rad * 0.5) * kAspect);
}

constexpr double kYaw = 0.6;
constexpr double kPitch = 0.45;

}  // namespace

TEST_CASE("gizmo on-screen size: pure dolly, pivot on the module") {
  const double f = kScreenH / (2.0 * std::tan(kFovYDeg * kDeg2Rad * 0.5));
  const Vec3 module{0, 0, 0};

  double minPx = 1e30;
  double maxPx = 0.0;
  for (const double dist : {25.0, 50.0, 100.0, 400.0, 1600.0, 6400.0}) {
    const Vec3 eye = orbitEye(module, dist, kYaw, kPitch);
    const CameraBasis b = cameraBasis(module - eye, Vec3{0, 1, 0});
    const double px = gizmoPixelRadius(module, scaleFor(module, eye, b), eye, b, f);
    minPx = std::min(minPx, px);
    maxPx = std::max(maxPx, px);
  }
  CHECK(maxPx / minPx < 1.02);  // centred handle: exactly constant on screen
}

TEST_CASE("gizmo on-screen size: size depends only on the eye->part distance, not the pivot") {
  // The fix's core invariant: the handle size is a function of the part's own eye distance
  // alone. Orbiting a centred part at a FIXED eye distance from many yaw/pitch angles (the
  // pivot stays on the part, so the orbit distance equals the eye distance) must hold the
  // handle constant — and pulling the EYE in/out must scale the world handle but keep its
  // pixels constant. The old orbit-keyed floor broke this whenever the pivot diverged.
  const double f = kScreenH / (2.0 * std::tan(kFovYDeg * kDeg2Rad * 0.5));
  const Vec3 module{0, 0, 0};

  double minPx = 1e30;
  double maxPx = 0.0;
  for (const double dist : {120.0, 800.0, 5000.0}) {
    for (const double yaw : {-2.0, -0.3, 0.6, 1.9}) {
      for (const double pitch : {-1.2, 0.0, 0.45, 1.2}) {
        const Vec3 eye = orbitEye(module, dist, yaw, pitch);
        const CameraBasis b = cameraBasis(module - eye, Vec3{0, 1, 0});
        const double px = gizmoPixelRadius(module, scaleFor(module, eye, b), eye, b, f);
        minPx = std::min(minPx, px);
        maxPx = std::max(maxPx, px);
      }
    }
  }
  // <1.10: the residual is the perspective foreshortening of the 3D handle's finite arms
  // as the view angle changes (inherent to any 3D gizmo) — NOT the orbit-scaling bug, which
  // moved the handle ~2.15x. Pure distance change at a fixed angle stays <1.02 (dolly case).
  CHECK(maxPx / minPx < 1.10);
}

TEST_CASE("gizmo on-screen size: zoom-toward-cursor keeps the handle near-constant") {
  // Realistic gesture: a stationary cursor 30% off screen-centre while wheeling in. The
  // pivot drifts and the part slides toward the screen edge. Within the central region
  // (|ndcX| <= 0.4) the handle must stay near-constant; the small residual growth toward
  // the screen edge is the inherent rectilinear edge-stretch every flat-screen 3D editor
  // has (Blender/Unreal included), not the orbit-coupling bug.
  const double f = kScreenH / (2.0 * std::tan(kFovYDeg * kDeg2Rad * 0.5));
  const Vec3 module{0, 0, 0};
  const double cursorNdcX = 0.30;

  Vec3 target = module;
  double distance = 3000.0;
  double minPx = 1e30;
  double maxPx = 0.0;
  for (int step = 0; step < 16; ++step) {
    const Vec3 eye = orbitEye(target, distance, kYaw, kPitch);
    const CameraBasis b = cameraBasis(target - eye, Vec3{0, 1, 0});
    const double px = gizmoPixelRadius(module, scaleFor(module, eye, b), eye, b, f);
    if (std::abs(moduleNdcX(module, eye, b)) <= 0.4) {
      minPx = std::min(minPx, px);
      maxPx = std::max(maxPx, px);
    }
    const Vec3 rayDir = x4sb::editor::normalized(
        b.forward + b.right * (cursorNdcX * std::tan(kFovYDeg * kDeg2Rad * 0.5) * kAspect));
    const ZoomResult z =
        zoomTowardCursor(target, distance, b.forward, eye, rayDir, 0.9, 2.0, 40000.0);
    target = z.target;
    distance = z.distance;
  }
  CHECK(maxPx / minPx < 1.25);  // on-screen core: only the inherent edge-stretch remains
}

TEST_CASE("gizmo on-screen size: big-station, module far off the pivot, stays near-constant") {
  // The user's case: a module 6 km off the station centre, framed from near (zoomed in on it)
  // to far (whole station in view, module off to the side). Without the cap this is the pure
  // constant-pixel path; while the module is on-screen the handle must stay near-constant.
  const double f = kScreenH / (2.0 * std::tan(kFovYDeg * kDeg2Rad * 0.5));
  const Vec3 module{6000, 0, 0};
  const Vec3 stationCenter{0, 0, 0};
  double minPx = 1e30;
  double maxPx = 0.0;
  for (const double dist : {200.0, 1000.0, 4000.0, 10000.0, 20000.0, 40000.0}) {
    // Lerp the pivot from the module (zoomed in) to the station centre (zoomed out), the way
    // a user re-centres as they pull back.
    const double t = std::min(1.0, dist / 20000.0);
    const Vec3 target = module * (1.0 - t) + stationCenter * t;
    const Vec3 eye = orbitEye(target, dist, kYaw, kPitch);
    const CameraBasis b = cameraBasis(target - eye, Vec3{0, 1, 0});
    if (std::abs(moduleNdcX(module, eye, b)) > 1.0) continue;  // off-screen: not user-visible
    const double px = gizmoPixelRadius(module, scaleFor(module, eye, b), eye, b, f);
    minPx = std::min(minPx, px);
    maxPx = std::max(maxPx, px);
  }
  CHECK(maxPx / minPx < 1.15);  // on-screen: constant but for the inherent angle/edge residual
}

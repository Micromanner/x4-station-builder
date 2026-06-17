#include "x4sb/editorcore/gizmo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace x4sb {
namespace {

// Closest distance between a ray (o + t*d, t>=0) and a finite segment [a,b],
// with the ray parameter at that closest point. Standard two-line solve, clamped
// to the segment and to t>=0.
struct RaySegResult {
  double dist;
  double rayT;
};
RaySegResult rayToSegment(Vec3 o, Vec3 d, Vec3 a, Vec3 b) {
  const Vec3 v = b - a;
  const Vec3 w0 = o - a;
  const double aa = dot(d, d);
  const double bb = dot(d, v);
  const double cc = dot(v, v);
  const double dd = dot(d, w0);
  const double ee = dot(v, w0);
  const double denom = aa * cc - bb * bb;
  double tRay = 0.0;
  double sSeg = 0.0;
  if (denom > 1e-12) {
    tRay = (bb * ee - cc * dd) / denom;
    sSeg = (aa * ee - bb * dd) / denom;
  } else {
    tRay = aa > 1e-12 ? -dd / aa : 0.0;
  }
  if (tRay < 0.0) tRay = 0.0;
  sSeg = std::clamp(sSeg, 0.0, 1.0);
  const Vec3 pRay = o + d * tRay;
  const Vec3 pSeg = a + v * sSeg;
  return {length(pRay - pSeg), tRay};
}

// Non-negative ray parameter where the ray crosses the plane through `p` with
// normal `n`, or nullopt (parallel or behind).
std::optional<double> rayPlaneT(Vec3 o, Vec3 d, Vec3 p, Vec3 n) {
  const double denom = dot(d, n);
  if (std::abs(denom) < 1e-12) return std::nullopt;
  const double t = dot(p - o, n) / denom;
  if (t < 0.0) return std::nullopt;
  return t;
}

// The two in-plane basis axes for a plane handle (the axes that aren't its normal).
void planeBasis(GizmoHandle h, Vec3& u, Vec3& v) {
  switch (h) {
    case GizmoHandle::PlaneXY:
      u = {1, 0, 0};
      v = {0, 1, 0};
      break;
    case GizmoHandle::PlaneYZ:
      u = {0, 1, 0};
      v = {0, 0, 1};
      break;
    default:  // PlaneZX
      u = {0, 0, 1};
      v = {1, 0, 0};
      break;
  }
}

std::optional<double> raySphereT(Vec3 o, Vec3 d, Vec3 center, double radius) {
  const Vec3 oc = o - center;
  const double a = dot(d, d);
  const double b = 2.0 * dot(oc, d);
  const double c = dot(oc, oc) - radius * radius;
  const double discriminant = b * b - 4.0 * a * c;
  if (discriminant < 0.0) return std::nullopt;
  const double t = (-b - std::sqrt(discriminant)) / (2.0 * a);
  if (t < 0.0) {
    const double t2 = (-b + std::sqrt(discriminant)) / (2.0 * a);
    if (t2 >= 0.0) return 0.0;
    return std::nullopt;
  }
  return t;
}

}  // namespace

GizmoModel gizmoModel(Vec3 origin, double scale) {
  GizmoModel g;
  g.origin = origin;
  g.axisLength = scale;
  g.planeSize = 0.3 * scale;
  g.axisPickRadius = 0.08 * scale;
  g.ringRadius = scale;  // rings circumscribe the arrow tips
  g.centerPickRadius = 0.15 * scale;
  return g;
}

double gizmoScale(double moduleRadius, double camDistance) {
  // World-relative: arms reach ~1.3x the module's bounding radius so the handles
  // sit just outside the mesh, and the whole gizmo grows/shrinks WITH the module
  // (the user's complaint: a screen-constant gizmo looked like it grew on zoom-out).
  constexpr double kReach = 1.3;
  // Screen-relative guards (fractions of camera distance): a floor so a tiny module
  // is still grabbable, a ceiling so a huge module's gizmo never fills the view.
  constexpr double kMinFrac = 0.06;
  constexpr double kMaxFrac = 0.7;
  const double d = camDistance > 0.0 ? camDistance : 0.0;
  return std::clamp(moduleRadius * kReach, d * kMinFrac, d * kMaxFrac);
}

bool gizmoIsAxis(GizmoHandle h) {
  return h == GizmoHandle::AxisX || h == GizmoHandle::AxisY || h == GizmoHandle::AxisZ;
}

bool gizmoIsRotation(GizmoHandle h) {
  return h == GizmoHandle::RotX || h == GizmoHandle::RotY || h == GizmoHandle::RotZ;
}

Vec3 gizmoAxisDir(GizmoHandle h) {
  switch (h) {
    case GizmoHandle::AxisX:
    case GizmoHandle::RotX:
      return {1, 0, 0};
    case GizmoHandle::AxisY:
    case GizmoHandle::RotY:
      return {0, 1, 0};
    case GizmoHandle::AxisZ:
    case GizmoHandle::RotZ:
      return {0, 0, 1};
    default:
      return {0, 0, 0};
  }
}

Vec3 gizmoPlaneNormal(GizmoHandle h) {
  switch (h) {
    case GizmoHandle::PlaneXY:
      return {0, 0, 1};
    case GizmoHandle::PlaneYZ:
      return {1, 0, 0};
    case GizmoHandle::PlaneZX:
      return {0, 1, 0};
    default:
      return {0, 0, 0};
  }
}

std::optional<GizmoHandle> gizmoPick(const GizmoModel& g, Vec3 rayOrigin, Vec3 rayDir) {
  std::optional<GizmoHandle> best;
  double bestT = std::numeric_limits<double>::infinity();

  // Center handle check
  const std::optional<double> tCenter = raySphereT(rayOrigin, rayDir, g.origin, g.centerPickRadius);
  if (tCenter && *tCenter < bestT) {
    bestT = *tCenter;
    best = GizmoHandle::Center;
  }

  // Plane handles first (priority): pick the nearest plane quad the ray enters.
  const std::array<GizmoHandle, 3> planes{GizmoHandle::PlaneXY, GizmoHandle::PlaneYZ,
                                          GizmoHandle::PlaneZX};
  for (const GizmoHandle h : planes) {
    const std::optional<double> t = rayPlaneT(rayOrigin, rayDir, g.origin, gizmoPlaneNormal(h));
    if (!t) continue;
    const Vec3 hit = rayOrigin + rayDir * (*t);
    Vec3 u;
    Vec3 v;
    planeBasis(h, u, v);
    const double pu = dot(hit - g.origin, u);
    const double pv = dot(hit - g.origin, v);
    if (pu >= 0.0 && pu <= g.planeSize && pv >= 0.0 && pv <= g.planeSize && *t < bestT) {
      bestT = *t;
      best = h;
    }
  }
  if (best) return best;

  // Axis handles and rotation rings compete on nearest ray distance.
  const std::array<GizmoHandle, 3> axes{GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ};
  for (const GizmoHandle h : axes) {
    const Vec3 a = g.origin;
    const Vec3 b = g.origin + gizmoAxisDir(h) * g.axisLength;
    const RaySegResult r = rayToSegment(rayOrigin, rayDir, a, b);
    if (r.dist <= g.axisPickRadius && r.rayT < bestT) {
      bestT = r.rayT;
      best = h;
    }
  }

  // Rotation rings: a circle of radius ringRadius in the plane normal to each axis.
  // Hit when the ray crosses that plane within axisPickRadius of the ring radius.
  const std::array<GizmoHandle, 3> rings{GizmoHandle::RotX, GizmoHandle::RotY, GizmoHandle::RotZ};
  for (const GizmoHandle h : rings) {
    const std::optional<double> t = rayPlaneT(rayOrigin, rayDir, g.origin, gizmoAxisDir(h));
    if (!t) continue;
    const Vec3 hit = rayOrigin + rayDir * (*t);
    const double r = length(hit - g.origin);
    if (std::abs(r - g.ringRadius) <= g.axisPickRadius && *t < bestT) {
      bestT = *t;
      best = h;
    }
  }
  return best;
}

Vec3 gizmoDragDelta(GizmoHandle handle, Vec3 origin, Vec3 startRayOrigin, Vec3 startRayDir,
                    Vec3 curRayOrigin, Vec3 curRayDir) {
  if (handle == GizmoHandle::Center) {
    const Vec3 n = startRayDir;
    const std::optional<double> tCur = rayPlaneT(curRayOrigin, curRayDir, origin, n);
    if (!tCur) return {0, 0, 0};
    const Vec3 hitCur = curRayOrigin + curRayDir * (*tCur);
    return hitCur - origin;
  }
  if (gizmoIsAxis(handle)) {
    const Vec3 axis = gizmoAxisDir(handle);
    // Project the mouse ray onto a camera-facing plane containing the drag axis.
    // Using the closest distance between skew lines (closestLineParamToRay) is
    // highly sensitive to camera view angles, causing sluggishness when looking perpendicular
    // to the axis, and hyper-sensitivity/singularities when looking parallel.
    // Projections onto a camera-facing plane avoid this and yield consistent mouse response.
    Vec3 n = startRayDir - axis * dot(startRayDir, axis);
    const double lenN = length(n);
    if (lenN < 1e-6) {
      if (std::abs(axis.x) < 0.5) {
        n = {1, 0, 0};
      } else if (std::abs(axis.y) < 0.5) {
        n = {0, 1, 0};
      } else {
        n = {0, 0, 1};
      }
      n = n - axis * dot(n, axis);
      const double len = length(n);
      n = len < 1e-12 ? Vec3{0, 0, 0} : n * (1.0 / len);
    } else {
      n = n * (1.0 / lenN);
    }

    auto projectRay = [&](Vec3 o, Vec3 d) -> double {
      const double denom = dot(d, n);
      if (std::abs(denom) < 1e-12) return 0.0;
      const double t = dot(origin - o, n) / denom;
      const Vec3 hit = o + d * t;
      return dot(hit - origin, axis);
    };

    const double sStart = projectRay(startRayOrigin, startRayDir);
    const double sCur = projectRay(curRayOrigin, curRayDir);
    return axis * (sCur - sStart);
  }
  const Vec3 n = gizmoPlaneNormal(handle);
  const std::optional<double> tStart = rayPlaneT(startRayOrigin, startRayDir, origin, n);
  const std::optional<double> tCur = rayPlaneT(curRayOrigin, curRayDir, origin, n);
  if (!tStart || !tCur) return {0, 0, 0};
  const Vec3 hitStart = startRayOrigin + startRayDir * (*tStart);
  const Vec3 hitCur = curRayOrigin + curRayDir * (*tCur);
  return hitCur - hitStart;
}

double gizmoDragRotation(GizmoHandle handle, Vec3 origin, Vec3 startRayOrigin, Vec3 startRayDir,
                         Vec3 curRayOrigin, Vec3 curRayDir) {
  const Vec3 n = gizmoAxisDir(handle);  // rotation axis = ring normal
  const std::optional<double> tStart = rayPlaneT(startRayOrigin, startRayDir, origin, n);
  const std::optional<double> tCur = rayPlaneT(curRayOrigin, curRayDir, origin, n);
  if (!tStart || !tCur) return 0.0;
  const Vec3 vStart = (startRayOrigin + startRayDir * (*tStart)) - origin;
  const Vec3 vCur = (curRayOrigin + curRayDir * (*tCur)) - origin;
  if (length(vStart) < 1e-9 || length(vCur) < 1e-9) return 0.0;
  // Signed angle from vStart to vCur about n (both lie in the ring plane).
  return std::atan2(dot(cross(vStart, vCur), n), dot(vStart, vCur));
}

}  // namespace x4sb

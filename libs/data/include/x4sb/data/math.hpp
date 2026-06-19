#pragma once
// Minimal, render-free math primitives shared across all libs.
// Deliberately independent of raylib so the logic units test headlessly;
// conversion to raylib's types happens only inside apps/editor.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace x4sb {

struct Vec3 {
  double x{0}, y{0}, z{0};
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(Vec3 a) { return std::sqrt(dot(a, a)); }

// Unit quaternion (w + xi + yj + zk). Default is identity.
struct Quat {
  double w{1}, x{0}, y{0}, z{0};
};

inline Quat operator*(Quat a, Quat b) {
  return {
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline Quat conjugate(Quat q) { return {q.w, -q.x, -q.y, -q.z}; }

// Rotate a vector by a (unit) quaternion: q * (0,v) * q^-1.
inline Vec3 rotate(Quat q, Vec3 v) {
  const Quat p{0, v.x, v.y, v.z};
  const Quat r = q * p * conjugate(q);
  return {r.x, r.y, r.z};
}

// Unit quaternion for a rotation of `radians` about `axis` (need not be unit).
// Identity if the axis is degenerate.
[[nodiscard]] inline Quat axisAngle(Vec3 axis, double radians) {
  const double len = length(axis);
  if (len < 1e-12) return {};
  const double half = radians * 0.5;
  const double s = std::sin(half) / len;
  return {std::cos(half), axis.x * s, axis.y * s, axis.z * s};
}

// Rigid-body transform: rotate, then translate. Default is identity.
struct Transform {
  Vec3 position{};
  Quat rotation{};
};

inline Vec3 apply(const Transform& t, Vec3 local) { return rotate(t.rotation, local) + t.position; }

// Compose two rigid transforms: the result applies `b` then `a` (i.e. `a` is the
// parent/world frame, `b` the child/local frame). Equivalent to
// apply(compose(a,b), v) == apply(a, apply(b, v)) for all v.
[[nodiscard]] inline Transform compose(const Transform& a, const Transform& b) {
  return {apply(a, b.position), a.rotation * b.rotation};
}

// Axis-aligned bounding box.
struct AABB {
  Vec3 min{};
  Vec3 max{};
};

inline bool overlaps(const AABB& a, const AABB& b) {
  return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
         a.min.z <= b.max.z && a.max.z >= b.min.z;
}

// Grow box in place to contain point p.
inline void expand(AABB& box, Vec3 p) {
  box.min = {std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
  box.max = {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
}

// Smallest box containing both a and b.
inline AABB merge(const AABB& a, const AABB& b) {
  return {{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
          {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

// Translate box by t (axes are unchanged, so the result stays axis-aligned).
inline AABB operator+(const AABB& b, Vec3 t) { return {b.min + t, b.max + t}; }

// World-space axis-aligned hull of a local box after a rigid transform: transform
// all 8 corners, then take their component-wise min/max. Conservative (>= the true
// rotated box), which is the safe direction for a collision-reject test.
[[nodiscard]] inline AABB worldAabb(const AABB& local, const Transform& t) {
  const std::array<Vec3, 8> corners{{
      {local.min.x, local.min.y, local.min.z}, {local.max.x, local.min.y, local.min.z},
      {local.min.x, local.max.y, local.min.z}, {local.max.x, local.max.y, local.min.z},
      {local.min.x, local.min.y, local.max.z}, {local.max.x, local.min.y, local.max.z},
      {local.min.x, local.max.y, local.max.z}, {local.max.x, local.max.y, local.max.z}}};
  const Vec3 c0 = apply(t, corners[0]);
  AABB out{c0, c0};
  for (std::size_t i = 1; i < corners.size(); ++i) expand(out, apply(t, corners[i]));
  return out;
}

// Oriented bounding box: half-extents about `center`, rotated by `rotation`.
struct Obb {
  Vec3 center{};
  Quat rotation{};
  Vec3 halfExtents{};
};

// Separating-axis test of an OBB against an axis-aligned box. True on overlap
// (touching counts). Candidate axes: the 3 AABB face normals (world X/Y/Z), the
// 3 OBB face normals, and the 9 edge cross-products. A near-zero cross-product
// (parallel edges) is skipped — its separation is already covered by a face axis.
[[nodiscard]] inline bool overlapsObbAabb(const Obb& obb, const AABB& aabb) {
  const Vec3 ac{(aabb.min.x + aabb.max.x) * 0.5, (aabb.min.y + aabb.max.y) * 0.5,
                (aabb.min.z + aabb.max.z) * 0.5};
  const Vec3 ah{(aabb.max.x - aabb.min.x) * 0.5, (aabb.max.y - aabb.min.y) * 0.5,
                (aabb.max.z - aabb.min.z) * 0.5};
  const Vec3 d = obb.center - ac;  // center-to-center
  const std::array<Vec3, 3> ax{{rotate(obb.rotation, {1, 0, 0}), rotate(obb.rotation, {0, 1, 0}),
                                rotate(obb.rotation, {0, 0, 1})}};
  const std::array<Vec3, 3> wb{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};  // AABB axes
  const std::array<double, 3> he{{obb.halfExtents.x, obb.halfExtents.y, obb.halfExtents.z}};

  // Projected radius of the AABB onto axis L.
  const auto radiusAabb = [&](const Vec3& L) {
    return std::abs(L.x) * ah.x + std::abs(L.y) * ah.y + std::abs(L.z) * ah.z;
  };
  // Projected radius of the OBB onto axis L.
  const auto radiusObb = [&](const Vec3& L) {
    return std::abs(dot(L, ax[0])) * he[0] + std::abs(dot(L, ax[1])) * he[1] +
           std::abs(dot(L, ax[2])) * he[2];
  };
  const auto separated = [&](const Vec3& L) {
    if (dot(L, L) < 1e-12) return false;  // degenerate axis: not separating
    return std::abs(dot(d, L)) > radiusAabb(L) + radiusObb(L);
  };

  for (const Vec3& L : wb)
    if (separated(L)) return false;
  for (const Vec3& L : ax)
    if (separated(L)) return false;
  for (const Vec3& A : wb)
    for (const Vec3& B : ax)
      if (separated(cross(A, B))) return false;
  return true;
}

}  // namespace x4sb

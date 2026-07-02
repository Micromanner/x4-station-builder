#include "x4sb/data/math.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

TEST_CASE("identity quaternion leaves a vector unchanged") {
  const Vec3 r = rotate(Quat{}, Vec3{1, 2, 3});
  CHECK(r.x == doctest::Approx(1));
  CHECK(r.y == doctest::Approx(2));
  CHECK(r.z == doctest::Approx(3));
}

TEST_CASE("90 degree rotation about Z maps +X to +Y") {
  // q for 90deg about Z: (cos45, 0, 0, sin45)
  const double s = std::sqrt(2.0) / 2.0;
  const Vec3 r = rotate(Quat{s, 0, 0, s}, Vec3{1, 0, 0});
  CHECK(r.x == doctest::Approx(0).epsilon(1e-9));
  CHECK(r.y == doctest::Approx(1));
  CHECK(r.z == doctest::Approx(0).epsilon(1e-9));
}

TEST_CASE("compose matches applying both transforms in sequence") {
  // a: parent/world frame — translate then 90deg about Y.
  Transform a;
  a.position = Vec3{10, -5, 3};
  const double s = std::sqrt(2.0) / 2.0;
  a.rotation = Quat{s, 0, s, 0};  // 90deg about Y

  // b: child/local frame — different translation, 90deg about X.
  Transform b;
  b.position = Vec3{-2, 7, 4};
  b.rotation = Quat{s, s, 0, 0};  // 90deg about X

  const Transform ab = compose(a, b);

  // The defining identity: apply(compose(a,b), v) == apply(a, apply(b, v)).
  const std::array<Vec3, 4> samples{{Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{-3, 2, 5}}};
  for (const Vec3 v : samples) {
    const Vec3 viaCompose = apply(ab, v);
    const Vec3 viaSequence = apply(a, apply(b, v));
    CHECK(viaCompose.x == doctest::Approx(viaSequence.x));
    CHECK(viaCompose.y == doctest::Approx(viaSequence.y));
    CHECK(viaCompose.z == doctest::Approx(viaSequence.z));
  }

  // Composing with identity on either side is a no-op when applied to points.
  const Transform left = compose(Transform{}, b);   // identity then b == b
  const Transform right = compose(a, Transform{});  // a then identity == a
  for (const Vec3 v : samples) {
    const Vec3 lc = apply(left, v);
    const Vec3 bv = apply(b, v);
    CHECK(lc.x == doctest::Approx(bv.x));
    CHECK(lc.y == doctest::Approx(bv.y));
    CHECK(lc.z == doctest::Approx(bv.z));

    const Vec3 rc = apply(right, v);
    const Vec3 av = apply(a, v);
    CHECK(rc.x == doctest::Approx(av.x));
    CHECK(rc.y == doctest::Approx(av.y));
    CHECK(rc.z == doctest::Approx(av.z));
  }
}

TEST_CASE("AABB overlap detection") {
  const AABB a{{0, 0, 0}, {2, 2, 2}};
  const AABB touching{{1, 1, 1}, {3, 3, 3}};
  const AABB apart{{5, 5, 5}, {6, 6, 6}};
  CHECK(overlaps(a, touching));
  CHECK_FALSE(overlaps(a, apart));
}

TEST_CASE("axisAngle: +90 deg about +Y sends +X to -Z") {
  const double kHalfPi = 1.5707963267948966;
  const Quat q90 = axisAngle(Vec3{0, 1, 0}, kHalfPi);
  const Vec3 r = rotate(q90, Vec3{1, 0, 0});
  CHECK(r.x == doctest::Approx(0).epsilon(1e-9));
  CHECK(r.y == doctest::Approx(0).epsilon(1e-9));
  CHECK(r.z == doctest::Approx(-1).epsilon(1e-9));
}

TEST_CASE("overlapsObbAabb: axis-aligned, touching, separated, and rotated cases") {
  const AABB unit{{-1, -1, -1}, {1, 1, 1}};  // centered at origin

  // Concentric, axis-aligned OBB -> overlap.
  CHECK(overlapsObbAabb(Obb{{0, 0, 0}, Quat{}, {1, 1, 1}}, unit));

  // Far away on +X -> separated.
  CHECK_FALSE(overlapsObbAabb(Obb{{10, 0, 0}, Quat{}, {1, 1, 1}}, unit));

  // Axis-aligned, faces exactly touching at x=2 (gap 0) -> overlap (touching counts).
  CHECK(overlapsObbAabb(Obb{{2, 0, 0}, Quat{}, {1, 1, 1}}, unit));

  // Just past contact -> separated.
  CHECK_FALSE(overlapsObbAabb(Obb{{2.001, 0, 0}, Quat{}, {1, 1, 1}}, unit));

  // Unit cube rotated 45deg about Z: its projected half-width on X is sqrt(2)~1.414.
  const Quat z45 = axisAngle(Vec3{0, 0, 1}, 0.7853981633974483);  // pi/4
  // Center at 2.3: nearest reach 2.3-1.414=0.886 < 1 -> overlaps the unit box.
  CHECK(overlapsObbAabb(Obb{{2.3, 0, 0}, z45, {1, 1, 1}}, unit));
  // Center at 2.5: nearest reach 2.5-1.414=1.086 > 1; the X axis separates -> no overlap.
  CHECK_FALSE(overlapsObbAabb(Obb{{2.5, 0, 0}, z45, {1, 1, 1}}, unit));
}

TEST_CASE("overlapsObbObb is tighter than AABB for rotated long boxes") {
  const Quat z45 = axisAngle(Vec3{0, 0, 1}, 0.7853981633974483);    // pi/4
  const AABB localLong{{-5, -0.5, -0.5}, {5, 0.5, 0.5}};            // long along local X
  const Vec3 perp{-std::sqrt(2.0) / 2.0, std::sqrt(2.0) / 2.0, 0};  // unit, perp to long axis in XY

  // Two parallel bars, both rotated 45deg about Z, offset perpendicular to their long
  // axis. Each bar's sqrt(2)-inflated AABB hull is a ~7.8-wide square, so the hulls
  // overlap, but the oriented bars are separated by a clean gap of 0.5.
  Transform ta;
  ta.rotation = z45;  // A centered at origin
  Transform tbClear;
  tbClear.rotation = z45;
  tbClear.position = perp * 1.5;  // perpendicular separation 1.5 > 0.5+0.5 -> bars do not touch

  // The conservative AABB hulls DO overlap — exactly the false positive OBB removes.
  CHECK(overlaps(worldAabb(localLong, ta), worldAabb(localLong, tbClear)));

  const Obb a{ta.position, ta.rotation, {5, 0.5, 0.5}};
  CHECK_FALSE(overlapsObbObb(a, Obb{tbClear.position, tbClear.rotation, {5, 0.5, 0.5}}));

  // Slide B in to a perpendicular separation of 0.5 (< 1.0) -> bars interpenetrate.
  CHECK(overlapsObbObb(a, Obb{perp * 0.5, z45, {5, 0.5, 0.5}}));

  // Identity sanity: concentric overlap; far apart separated; faces touching counts.
  CHECK(overlapsObbObb(Obb{{0, 0, 0}, Quat{}, {1, 1, 1}}, Obb{{0, 0, 0}, Quat{}, {1, 1, 1}}));
  CHECK_FALSE(
      overlapsObbObb(Obb{{0, 0, 0}, Quat{}, {1, 1, 1}}, Obb{{10, 0, 0}, Quat{}, {1, 1, 1}}));
  CHECK(overlapsObbObb(Obb{{0, 0, 0}, Quat{}, {1, 1, 1}}, Obb{{2, 0, 0}, Quat{}, {1, 1, 1}}));
}

TEST_CASE("twistAngle recovers a pure roll and ignores a pure swing") {
  const Vec3 zAxis{0, 0, 1};
  CHECK(twistAngle(axisAngle(zAxis, 1.2), zAxis) == doctest::Approx(1.2));
  CHECK(twistAngle(axisAngle(zAxis, -0.7), zAxis) == doctest::Approx(-0.7));
  CHECK(twistAngle(axisAngle(Vec3{1, 0, 0}, 1.0), zAxis) == doctest::Approx(0.0));
  CHECK(twistAngle(Quat{}, zAxis) == doctest::Approx(0.0));
  CHECK(twistAngle(axisAngle(zAxis, 1.0), Vec3{0, 0, 0}) == doctest::Approx(0.0));  // degenerate
}

TEST_CASE("rotateAbout pivots a transform around a world point") {
  Transform t;
  t.position = {2, 0, 0};
  const Transform out = rotateAbout(t, Vec3{1, 0, 0}, Vec3{0, 0, 1}, 1.5707963267948966);  // +90 Z
  CHECK(out.position.x == doctest::Approx(1));
  CHECK(out.position.y == doctest::Approx(1));
  CHECK(out.position.z == doctest::Approx(0));

  // A point that maps to the pivot before the roll still maps to it after.
  const Vec3 connectorLocal{-1, 0, 0};  // apply(t, connectorLocal) == (1,0,0) == pivot
  const Vec3 after = apply(out, connectorLocal);
  CHECK(after.x == doctest::Approx(1));
  CHECK(after.y == doctest::Approx(0));
  CHECK(after.z == doctest::Approx(0));
}

TEST_CASE("worldAabb rotates a box's hull conservatively") {
  const AABB local{{-1, -2, -3}, {1, 2, 3}};

  // Identity transform leaves the box unchanged.
  const AABB id = worldAabb(local, Transform{});
  CHECK(id.min.x == doctest::Approx(-1));
  CHECK(id.max.z == doctest::Approx(3));

  // 90deg about Z swaps the x/y extents (x-extent +-1 -> y, y-extent +-2 -> x).
  Transform rot;
  const double s = std::sqrt(2.0) / 2.0;
  rot.rotation = Quat{s, 0, 0, s};
  const AABB w = worldAabb(local, rot);
  CHECK(w.min.x == doctest::Approx(-2));
  CHECK(w.max.x == doctest::Approx(2));
  CHECK(w.min.y == doctest::Approx(-1));
  CHECK(w.max.y == doctest::Approx(1));
  CHECK(w.min.z == doctest::Approx(-3));
  CHECK(w.max.z == doctest::Approx(3));

  // Pure translation shifts the hull by the transform's position.
  Transform tr;
  tr.position = Vec3{10, 20, 30};
  const AABB t = worldAabb(local, tr);
  CHECK(t.min.x == doctest::Approx(9));   // -1 + 10
  CHECK(t.max.z == doctest::Approx(33));  //  3 + 30

  // 45deg about Z genuinely grows the hull (a naive extent-swap would not).
  Transform r45;
  const double h = 3.14159265358979323846 / 8.0;  // half of 45deg
  r45.rotation = Quat{std::cos(h), 0, 0, std::sin(h)};
  const AABB w45 = worldAabb(local, r45);
  CHECK(w45.max.x ==
        doctest::Approx(3.0 * std::sqrt(2.0) / 2.0));  // ~2.1213 > either original extent
}

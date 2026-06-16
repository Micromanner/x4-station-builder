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

TEST_CASE("AABB overlap detection") {
  const AABB a{{0, 0, 0}, {2, 2, 2}};
  const AABB touching{{1, 1, 1}, {3, 3, 3}};
  const AABB apart{{5, 5, 5}, {6, 6, 6}};
  CHECK(overlaps(a, touching));
  CHECK_FALSE(overlaps(a, apart));
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
  CHECK(w45.max.x == doctest::Approx(3.0 * std::sqrt(2.0) / 2.0));  // ~2.1213 > either original extent
}

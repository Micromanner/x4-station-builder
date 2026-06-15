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

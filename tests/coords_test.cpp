#include "x4sb/coords/coords.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>

using namespace x4sb;

TEST_CASE("position conversion is identity (app works in X4-native space)") {
  const Vec3 v{1.5, -2.0, 3.25};
  const Vec3 x4 = appToX4(v);
  CHECK(x4.x == doctest::Approx(1.5));
  CHECK(x4.y == doctest::Approx(-2.0));  // no axis swap: plan & connector space agree
  CHECK(x4.z == doctest::Approx(3.25));
  const Vec3 back = x4ToApp(x4);
  CHECK(back.x == doctest::Approx(v.x));
  CHECK(back.y == doctest::Approx(v.y));
  CHECK(back.z == doctest::Approx(v.z));
}

namespace {
// Rotate a unit vector and compare, so we test the *orientation* rather than the
// (non-unique) quaternion components.
void checkRot(const Quat& q, Vec3 in, Vec3 expect) {
  const Vec3 got = rotate(q, in);
  CHECK(got.x == doctest::Approx(expect.x).epsilon(1e-6));
  CHECK(got.y == doctest::Approx(expect.y).epsilon(1e-6));
  CHECK(got.z == doctest::Approx(expect.z).epsilon(1e-6));
}
}  // namespace

TEST_CASE("quatFromX4Euler: single-axis rotations point the expected way") {
  // yaw 90 about Y: +Z -> +X (right-handed about up).
  checkRot(quatFromX4Euler(90, 0, 0), {0, 0, 1}, {1, 0, 0});
  // pitch 90 about X: +Z -> -Y.
  checkRot(quatFromX4Euler(0, 90, 0), {0, 0, 1}, {0, -1, 0});
  // roll 90 about Z: +X -> +Y.
  checkRot(quatFromX4Euler(0, 0, 90), {1, 0, 0}, {0, 1, 0});
}

TEST_CASE("x4EulerFromQuat inverts quatFromX4Euler (lossless rotation round-trip)") {
  struct E {
    double yaw, pitch, roll;
  };
  for (E e : {E{0, 0, 0}, E{176.77042, 0, 0}, E{0, 0, 2.76366}, E{-3.22958, 0, 0}, E{30, 40, 50},
              E{-120, 15, -75}}) {
    const Quat q = quatFromX4Euler(e.yaw, e.pitch, e.roll);
    double yaw, pitch, roll;
    x4EulerFromQuat(q, yaw, pitch, roll);
    // Re-encode and compare orientations (angle triples can alias; orientation can't).
    const Quat q2 = quatFromX4Euler(yaw, pitch, roll);
    checkRot(q2, {1, 0, 0}, rotate(q, Vec3{1, 0, 0}));
    checkRot(q2, {0, 1, 0}, rotate(q, Vec3{0, 1, 0}));
    checkRot(q2, {0, 0, 1}, rotate(q, Vec3{0, 0, 1}));
  }
}

#include "x4sb/editorcore/display_flip.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

TEST_CASE("flipZ negates z, leaves x/y, and round-trips") {
  const Vec3 v{1.0, 2.0, 3.0};
  const Vec3 f = flipZ(v);
  CHECK(f.x == doctest::Approx(1.0));
  CHECK(f.y == doctest::Approx(2.0));
  CHECK(f.z == doctest::Approx(-3.0));

  const Vec3 rt = flipZ(flipZ(v));  // self-inverse
  CHECK(rt.x == doctest::Approx(v.x));
  CHECK(rt.y == doctest::Approx(v.y));
  CHECK(rt.z == doctest::Approx(v.z));
}

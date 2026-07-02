#include "x4sb/autolayout/metrics.hpp"
#include "x4sb/data/math.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace x4sb;

namespace {
const AABB kUnit{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};  // centered: body center == position

ModuleDef boxDef(const std::string& id, AABB box = kUnit) {
  ModuleDef d;
  d.id = id;
  d.aabb = box;
  return d;
}
PlacedModule at(const std::string& defId, Vec3 pos) {
  PlacedModule m;
  m.defId = defId;
  m.worldTransform.position = pos;
  return m;
}
}  // namespace

TEST_CASE("centroid is the mean of body centers; empty is the origin") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  Station empty;
  CHECK(centroid(empty, c).x == doctest::Approx(0.0));

  Station s;
  s.add(at("b", {2, 0, 0}));
  s.add(at("b", {-2, 0, 4}));
  const Vec3 ctr = centroid(s, c);
  CHECK(ctr.x == doctest::Approx(0.0));
  CHECK(ctr.y == doctest::Approx(0.0));
  CHECK(ctr.z == doctest::Approx(2.0));
}

TEST_CASE("radiusOfGyrationSq: mean squared distance from centroid; 0 for <=1 module") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  Station one;
  one.add(at("b", {5, 5, 5}));
  CHECK(radiusOfGyrationSq(one, c) == doctest::Approx(0.0));

  Station s;  // two unit boxes at x = +/-2 -> centroid origin, each dist^2 = 4
  s.add(at("b", {2, 0, 0}));
  s.add(at("b", {-2, 0, 0}));
  CHECK(radiusOfGyrationSq(s, c) == doctest::Approx(4.0));
}

TEST_CASE("bounds is the merged world AABB of all bodies") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  Station s;
  s.add(at("b", {2, 0, 0}));
  s.add(at("b", {-2, 0, 0}));
  const AABB b = bounds(s, c);
  CHECK(b.min.x == doctest::Approx(-2.5));
  CHECK(b.max.x == doctest::Approx(2.5));
}

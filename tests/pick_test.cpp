#include "x4sb/snap/pick.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace x4sb;

TEST_CASE("rayIntersectsAabb hits a box in front of the ray") {
  const AABB box{{-1, -1, -1}, {1, 1, 1}};
  const auto t = rayIntersectsAabb(Vec3{-5, 0, 0}, Vec3{1, 0, 0}, box);
  REQUIRE(t.has_value());
  CHECK(*t == doctest::Approx(4));  // enters x=-1 from x=-5
}

TEST_CASE("rayIntersectsAabb misses an off-axis box") {
  const AABB box{{-1, -1, -1}, {1, 1, 1}};
  CHECK_FALSE(rayIntersectsAabb(Vec3{-5, 5, 0}, Vec3{1, 0, 0}, box).has_value());
}

TEST_CASE("rayIntersectsAabb misses a box behind the ray") {
  const AABB box{{-1, -1, -1}, {1, 1, 1}};
  CHECK_FALSE(rayIntersectsAabb(Vec3{5, 0, 0}, Vec3{1, 0, 0}, box).has_value());
}

TEST_CASE("pickModule selects the nearest module the ray hits") {
  ModuleCatalog catalog;
  ModuleDef d;
  d.id = "M";
  d.aabb = AABB{{-1, -1, -1}, {1, 1, 1}};
  catalog.add(d);

  Station station;
  PlacedModule nearMod;
  nearMod.defId = "M";
  nearMod.worldTransform.position = {5, 0, 0};
  const InstanceId idNear = station.add(nearMod);
  PlacedModule farMod;
  farMod.defId = "M";
  farMod.worldTransform.position = {20, 0, 0};
  station.add(farMod);

  const auto hit = pickModule(station, catalog, Vec3{-5, 0, 0}, Vec3{1, 0, 0});
  REQUIRE(hit.has_value());
  CHECK(*hit == idNear);

  CHECK_FALSE(pickModule(station, catalog, Vec3{-5, 0, 0}, Vec3{-1, 0, 0}).has_value());
}

TEST_CASE("rayIntersectsAabb returns the exit distance when the origin is inside") {
  const AABB box{{-1, -1, -1}, {1, 1, 1}};
  const auto t = rayIntersectsAabb(Vec3{0, 0, 0}, Vec3{1, 0, 0}, box);
  REQUIRE(t.has_value());
  CHECK(*t == doctest::Approx(1));  // exits at x=1 from the origin
}

TEST_CASE("pickModule hits a module's rotated hull") {
  ModuleCatalog catalog;
  ModuleDef longDef;
  longDef.id = "L";
  longDef.aabb = AABB{{-5, -0.5, -0.5}, {5, 0.5, 0.5}};  // long along X
  catalog.add(longDef);

  // Rotated 90deg about Z, the long axis points along Y -> hull spans y[-5,5].
  Station station;
  PlacedModule m;
  m.defId = "L";
  const double s = std::sqrt(2.0) / 2.0;
  m.worldTransform.rotation = Quat{s, 0, 0, s};
  const InstanceId id = station.add(m);

  // Ray through (x=0, y=3): inside the ROTATED hull (y in [-5,5]) but outside the
  // unrotated box (y in [-0.5,0.5]).
  const auto hit = pickModule(station, catalog, Vec3{0, 3, -10}, Vec3{0, 0, 1});
  REQUIRE(hit.has_value());
  CHECK(*hit == id);

  // The same ray MISSES the identical module when it is NOT rotated.
  Station station2;
  PlacedModule unrot;
  unrot.defId = "L";  // identity rotation
  station2.add(unrot);
  CHECK_FALSE(pickModule(station2, catalog, Vec3{0, 3, -10}, Vec3{0, 0, 1}).has_value());
}

TEST_CASE("pickModule returns nullopt for empty station and unknown defId") {
  ModuleCatalog catalog;
  Station empty;
  CHECK_FALSE(pickModule(empty, catalog, Vec3{0, 0, 0}, Vec3{1, 0, 0}).has_value());

  // A placed module whose def isn't in the catalog is skipped (exercises if(!def)continue).
  Station station;
  PlacedModule ghost;
  ghost.defId = "missing";
  station.add(ghost);
  CHECK_FALSE(pickModule(station, catalog, Vec3{0, 0, 0}, Vec3{1, 0, 0}).has_value());
}

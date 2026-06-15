#include "x4sb/snap/snap.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

namespace {
ModuleDef makeModule(const std::string& id, const std::string& pointId, Vec3 pointPos) {
  ModuleDef d;
  d.id = id;
  ConnectionPoint cp;
  cp.id = pointId;
  cp.localPosition = pointPos;
  d.connectionPoints.push_back(cp);
  return d;
}
}  // namespace

TEST_CASE("empty station yields no candidate and no collision") {
  ModuleCatalog catalog;
  Station station;
  ModuleDef def = makeModule("A", "a1", {0, 0, 0});
  CHECK_FALSE(findSnapCandidate(def, Vec3{0, 0, 0}, station, catalog, 5.0).has_value());
  CHECK_FALSE(collidesWithStation(def, Transform{}, /*ignore=*/0, station, catalog));
}

TEST_CASE("computeSnapTransform makes the two connectors coincide (baseline)") {
  ModuleCatalog catalog;
  const ModuleDef a = makeModule("A", "a1", {1, 0, 0});
  const ModuleDef b = makeModule("B", "b1", {-1, 0, 0});
  catalog.add(a);
  catalog.add(b);

  Station station;
  PlacedModule pa;
  pa.defId = "A";
  const InstanceId idA = station.add(pa);

  const Transform xf = computeSnapTransform(station, catalog, idA, "a1", b, "b1");

  const Vec3 targetWorld = apply(station.find(idA)->worldTransform, Vec3{1, 0, 0});
  const Vec3 newWorld = apply(xf, Vec3{-1, 0, 0});
  CHECK(newWorld.x == doctest::Approx(targetWorld.x));
  CHECK(newWorld.y == doctest::Approx(targetWorld.y));
  CHECK(newWorld.z == doctest::Approx(targetWorld.z));
}

TEST_CASE("findSnapCandidate locates a nearby free connector") {
  ModuleCatalog catalog;
  catalog.add(makeModule("A", "a1", {0, 0, 0}));
  const ModuleDef newDef = makeModule("B", "b1", {0, 0, 0});

  Station station;
  PlacedModule pa;
  pa.defId = "A";
  pa.worldTransform.position = {10, 0, 0};
  station.add(pa);

  const auto hit = findSnapCandidate(newDef, Vec3{10.2, 0, 0}, station, catalog, 1.0);
  REQUIRE(hit.has_value());
  CHECK(hit->targetPointId == "a1");
  CHECK(hit->newPointId == "b1");

  const auto miss = findSnapCandidate(newDef, Vec3{0, 0, 0}, station, catalog, 1.0);
  CHECK_FALSE(miss.has_value());
}

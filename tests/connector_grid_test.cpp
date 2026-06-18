#include "x4sb/snap/connector_grid.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

namespace {
ModuleDef moduleWithConnectors(const std::string& id, const std::vector<Vec3>& localPositions) {
  ModuleDef d;
  d.id = id;
  for (std::size_t i = 0; i < localPositions.size(); ++i) {
    ConnectionPoint cp;
    cp.id = "c" + std::to_string(i);
    cp.localPosition = localPositions[i];
    d.connectionPoints.push_back(cp);
  }
  return d;
}
}  // namespace

TEST_CASE("ConnectorGrid stores every connector and finds points in range") {
  ModuleCatalog catalog;
  catalog.add(moduleWithConnectors("A", {Vec3{0, 0, 0}, Vec3{100, 0, 0}}));

  Station station;
  PlacedModule pm;
  pm.defId = "A";
  pm.worldTransform.position = {1000, 0, 0};
  const InstanceId id = station.add(pm);

  const ConnectorGrid grid(station, catalog, 50.0);
  CHECK(grid.size() == 2);

  // World positions: c0 = {1000,0,0}, c1 = {1100,0,0}.
  const auto hitC0 = grid.queryRadius(Vec3{1000, 0, 0}, 10.0);
  REQUIRE(hitC0.size() == 1);
  CHECK(hitC0[0].instanceId == id);
  CHECK(hitC0[0].connectorIndex == 0);

  CHECK(grid.queryRadius(Vec3{1050, 0, 0}, 1000.0).size() == 2);
  CHECK(grid.queryRadius(Vec3{0, 0, 0}, 100.0).empty());
}

TEST_CASE("ConnectorGrid skips modules whose def is missing") {
  ModuleCatalog catalog;  // empty -> no def for "A"
  Station station;
  PlacedModule pm;
  pm.defId = "A";
  station.add(pm);

  const ConnectorGrid grid(station, catalog, 50.0);
  CHECK(grid.size() == 0);
  CHECK(grid.queryRadius(Vec3{0, 0, 0}, 1000.0).empty());
}

TEST_CASE("ConnectorGrid query visits only nearby connectors") {
  ModuleCatalog catalog;
  catalog.add(moduleWithConnectors("A", {Vec3{0, 0, 0}}));

  Station station;
  for (int i = 0; i < 500; ++i) {
    PlacedModule pm;
    pm.defId = "A";
    pm.worldTransform.position = {static_cast<double>(i) * 2000.0, 0, 0};
    station.add(pm);
  }
  const ConnectorGrid grid(station, catalog, 1000.0);
  CHECK(grid.size() == 500);

  // Spaced 2000 apart with a 1000 radius -> at most the one at 200000 is in range.
  const auto near = grid.queryRadius(Vec3{200000.0, 0, 0}, 1000.0);
  CHECK(near.size() == 1);  // only the module at 200000; neighbors at +/-2000 are out of range
}

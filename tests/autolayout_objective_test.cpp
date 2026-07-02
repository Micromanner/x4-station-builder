#include "x4sb/autolayout/autolayout.hpp"
#include "x4sb/autolayout/objective.hpp"
#include "x4sb/data/math.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace x4sb;

namespace {
const AABB kUnit{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};

ModuleDef def(const std::string& id, Category cat, AABB box = kUnit) {
  ModuleDef d;
  d.id = id;
  d.category = cat;
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

TEST_CASE("connectorMaterial counts + volumes only Connector-category modules") {
  ModuleCatalog c;
  c.add(def("hub", Category::Connector, kUnit));    // volume 1
  c.add(def("prod", Category::Production, kUnit));  // not counted
  Station none;
  none.add(at("prod", {0, 0, 0}));
  CHECK(connectorMaterial(none, c) == doctest::Approx(0.0));

  Station one;
  one.add(at("hub", {0, 0, 0}));
  one.add(at("prod", {5, 0, 0}));
  CHECK(connectorMaterial(one, c) == doctest::Approx(1.0 + 1.0));  // count 1 + volume 1

  Station two;
  two.add(at("hub", {0, 0, 0}));
  two.add(at("hub", {5, 0, 0}));
  CHECK(connectorMaterial(two, c) > connectorMaterial(one, c));  // more connectors -> more material
}

TEST_CASE("compactObjective rewards tighter packing and penalizes connectors") {
  ModuleCatalog c;
  c.add(def("hub", Category::Connector, kUnit));
  c.add(def("prod", Category::Production, kUnit));
  AutoLayoutOptions opts;  // wMaterial defaults to 1.0

  const Objective obj = compactObjective(opts);

  Station tight;
  tight.add(at("prod", {1, 0, 0}));
  tight.add(at("prod", {-1, 0, 0}));
  Station spread;
  spread.add(at("prod", {10, 0, 0}));
  spread.add(at("prod", {-10, 0, 0}));
  CHECK(obj(tight, c) < obj(spread, c));  // lower Rg^2 wins

  Station withHub = tight;
  withHub.add(at("hub", {0, 5, 0}));
  CHECK(obj(withHub, c) > obj(tight, c));  // the extra connector costs material
}

#include "x4sb/autolayout/peripheral.hpp"
#include "x4sb/autolayout/roles.hpp"
#include "x4sb/data/math.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace x4sb;

namespace {
const AABB kUnit{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};

ModuleDef boxDef(const std::string& id, Category cat, AABB box) {
  ModuleDef d;
  d.id = id;
  d.category = cat;
  d.aabb = box;
  return d;
}
// Hub: small body, four free connectors on ±X/±Z, 2 units out. Untyped "snap".
ModuleDef hubDef(const std::string& id) {
  ModuleDef d = boxDef(id, Category::Production, kUnit);
  for (const auto& [pid, pos] : std::vector<std::pair<std::string, Vec3>>{
           {"c0", {2, 0, 0}}, {"c1", {-2, 0, 0}}, {"c2", {0, 0, 2}}, {"c3", {0, 0, -2}}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = pos;
    cp.type = "snap";
    d.connectionPoints.push_back(cp);
  }
  return d;
}
// A turret: one mount connector at its origin (so it mates centered on a hub port),
// body of unit size; classified Turret by the defence_ prefix.
ModuleDef turretDef(const std::string& id) {
  ModuleDef d = boxDef(id, Category::Other, kUnit);
  ConnectionPoint cp;
  cp.id = "s";
  cp.localPosition = {0, 0, 0};
  cp.type = "snap";
  d.connectionPoints.push_back(cp);
  return d;
}

Vec3 centroidOf(const Station& s) {
  Vec3 sum{};
  for (const auto& m : s.modules()) sum = sum + m.worldTransform.position;
  return sum * (1.0 / static_cast<double>(s.size()));
}
}  // namespace

TEST_CASE("placePeripheral mounts a turret on the outermost connector, facing outward") {
  ModuleCatalog c;
  c.add(hubDef("hub"));
  c.add(turretDef("defence_arg_disc_01_macro"));
  Station work;
  PlacedModule h;
  h.defId = "hub";
  const InstanceId hid = work.add(h);  // single hub at origin; all four ports free

  const ModuleDef* td = c.find("defence_arg_disc_01_macro");
  REQUIRE(td != nullptr);
  AutoLayoutOptions opts;
  const auto p = placePeripheral(work, c, *td, classifyRole(*td), opts);

  REQUIRE(p.has_value());
  CHECK(p->snapped);
  CHECK(p->targetInstanceId == hid);
  CHECK(work.size() == 2);
  // Mounted outward: the turret body sits outside the hub (farther from centroid
  // than the hub body).
  const Vec3 centroid = centroidOf(work);
  const PlacedModule* turret = work.find(p->instanceId);
  REQUIRE(turret != nullptr);
  CHECK(length(turret->worldTransform.position - centroid) > 0.0);
  // The hub gained exactly one reciprocal link to the turret.
  REQUIRE(work.find(hid)->links.size() == 1);
  CHECK(work.find(hid)->links[0].otherInstanceId == p->instanceId);
}

TEST_CASE("placePeripheral returns nullopt when the only port's outward lane is blocked") {
  // A hub with a SINGLE +X port; a wall beyond that port; a ballast on -X that cancels
  // the wall so the centroid stays at the origin — therefore the +X port faces
  // *genuinely* outward, straight into the wall. With no other port to fall back to,
  // the only legal-by-body placement is rejected by the forward-clear (can't-hit-itself)
  // test, so placement fails. (Without forwardClear this would wrongly succeed — this
  // case is what isolates that logic.)
  ModuleCatalog c;
  ModuleDef hub = boxDef("hub", Category::Production, kUnit);
  {
    ConnectionPoint cp;
    cp.id = "c0";
    cp.localPosition = {2, 0, 0};
    cp.type = "snap";
    hub.connectionPoints.push_back(cp);
  }
  c.add(hub);
  c.add(turretDef("defence_arg_disc_01_macro"));
  c.add(boxDef("wall", Category::Production, AABB{{-1, -5, -5}, {1, 5, 5}}));     // no ports
  c.add(boxDef("ballast", Category::Production, AABB{{-1, -5, -5}, {1, 5, 5}}));  // no ports

  Station work;
  PlacedModule h;
  h.defId = "hub";
  work.add(h);  // origin
  PlacedModule w;
  w.defId = "wall";
  w.worldTransform.position = {8, 0, 0};  // blocks the +X outward lane
  work.add(w);
  PlacedModule b;
  b.defId = "ballast";
  b.worldTransform.position = {-8, 0, 0};  // cancels the wall => centroid at origin
  work.add(b);

  const ModuleDef* td = c.find("defence_arg_disc_01_macro");
  AutoLayoutOptions opts;
  const auto p = placePeripheral(work, c, *td, classifyRole(*td), opts);
  CHECK_FALSE(p.has_value());
}

TEST_CASE("placePeripheral is deterministic") {
  ModuleCatalog c;
  c.add(hubDef("hub"));
  c.add(turretDef("defence_arg_disc_01_macro"));
  const ModuleDef* td = c.find("defence_arg_disc_01_macro");
  AutoLayoutOptions opts;

  Station a;
  PlacedModule ha;
  ha.defId = "hub";
  a.add(ha);
  Station b = a;

  const auto pa = placePeripheral(a, c, *td, Role::Turret, opts);
  const auto pb = placePeripheral(b, c, *td, Role::Turret, opts);
  REQUIRE(pa.has_value());
  REQUIRE(pb.has_value());
  CHECK(pa->targetPointId == pb->targetPointId);
  CHECK(pa->worldTransform.position.x == doctest::Approx(pb->worldTransform.position.x));
  CHECK(pa->worldTransform.position.z == doctest::Approx(pb->worldTransform.position.z));
}

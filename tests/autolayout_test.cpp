#include "x4sb/autolayout/autolayout.hpp"

#include "x4sb/snap/snap.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace x4sb;

namespace {
// A module with two cube bodies' worth of footprint and a configurable category.
ModuleDef boxDef(const std::string& id, Category cat, AABB box, bool buildable = true) {
  ModuleDef d;
  d.id = id;
  d.category = cat;
  d.aabb = box;
  d.playerBuildable = buildable;
  return d;
}
const AABB kUnit{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};
const AABB kBig{{-2, -2, -2}, {2, 2, 2}};

// A "hub": small body, four free connectors on the ±X/±Z axes, 2 units out (well
// beyond the 0.5 body) so a child mated there sits clear of the hub. Untyped.
ModuleDef hubDef(const std::string& id, Category cat) {
  ModuleDef d = boxDef(id, cat, kUnit);
  for (const auto& [pid, pos] : std::vector<std::pair<std::string, Vec3>>{
           {"c0", {2, 0, 0}}, {"c1", {-2, 0, 0}}, {"c2", {0, 0, 2}}, {"c3", {0, 0, -2}}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = pos;
    d.connectionPoints.push_back(cp);
  }
  return d;
}
// A "spoke": small body with ONE connector at its own origin, so mating it onto a hub
// connector places the spoke's centre exactly at that connector (clear of the hub body).
ModuleDef spokeDef(const std::string& id, Category cat, const std::string& type = "") {
  ModuleDef d = boxDef(id, cat, kUnit);
  ConnectionPoint cp;
  cp.id = "s";
  cp.localPosition = {0, 0, 0};
  cp.type = type;
  d.connectionPoints.push_back(cp);
  return d;
}

// True if no two NON-adjacent module bodies overlap (directly-linked modules touch at
// the joint by construction, so linked neighbours are ignored).
bool noNonAdjacentOverlap(const Station& s, const ModuleCatalog& c) {
  for (const auto& m : s.modules()) {
    const ModuleDef* dm = c.find(m.defId);
    if (dm == nullptr) continue;
    const AABB am = worldAabb(dm->aabb, m.worldTransform);
    for (const auto& o : s.modules()) {
      if (o.instanceId == m.instanceId) continue;
      const bool linked = std::any_of(m.links.begin(), m.links.end(), [&](const Link& l) {
        return l.otherInstanceId == o.instanceId;
      });
      if (linked) continue;
      const ModuleDef* doo = c.find(o.defId);
      if (doo == nullptr) continue;
      if (overlaps(am, worldAabb(doo->aabb, o.worldTransform))) return false;
    }
  }
  return true;
}

bool insidePlotBox(const AABB& b, double h) {
  return b.min.x >= -h && b.max.x <= h && b.min.y >= -h && b.max.y <= h && b.min.z >= -h &&
         b.max.z <= h;
}
}  // namespace

TEST_CASE("orderedPlacement: category priority, then largest-first, then id; skips bad defs") {
  ModuleCatalog c;
  c.add(boxDef("dock1", Category::Dock, kUnit));
  c.add(boxDef("prod_small", Category::Production, kUnit));
  c.add(boxDef("prod_big", Category::Production, kBig));
  c.add(boxDef("conn1", Category::Connector, kUnit));
  ModuleDef nb = boxDef("nb", Category::Production, kUnit, /*buildable=*/false);
  c.add(nb);

  const OrderedCart oc = orderedPlacement(
      QuantityList{
          {"dock1", 1}, {"prod_small", 1}, {"prod_big", 1}, {"conn1", 1}, {"nb", 2}, {"ghost", 3}},
      c);

  CHECK(oc.requested == 9);  // 1+1+1+1+2+3
  // Connector first, then Production big-before-small, then Dock last.
  REQUIRE(oc.placement.size() == 4);
  CHECK(oc.placement[0] == "conn1");
  CHECK(oc.placement[1] == "prod_big");
  CHECK(oc.placement[2] == "prod_small");
  CHECK(oc.placement[3] == "dock1");
  // Unknown ("ghost") and non-buildable ("nb") are skipped (deduped, sorted).
  CHECK(oc.skipped == std::vector<std::string>{"ghost", "nb"});
}

TEST_CASE("autoLayout greenfield: everything placed, no non-adjacent overlap, in plot") {
  ModuleCatalog c;
  c.add(hubDef("hub", Category::Production));
  c.add(spokeDef("spoke", Category::Production));
  Station empty;

  const AutoLayoutResult r = autoLayout(empty, QuantityList{{"hub", 1}, {"spoke", 3}}, c);

  CHECK(r.report.requested == 4);
  CHECK(r.report.skipped == 0);
  CHECK(r.report.snapped + r.report.floating == 4);
  CHECK(r.station.size() == 4);
  CHECK(r.placements.size() == 4);
  CHECK(noNonAdjacentOverlap(r.station, c));
  for (const auto& m : r.station.modules()) {
    const ModuleDef* d = c.find(m.defId);
    REQUIRE(d != nullptr);
    CHECK(insidePlotBox(worldAabb(d->aabb, m.worldTransform), 10000.0));
  }
}

TEST_CASE("autoLayout floats a module that cannot legally snap") {
  ModuleCatalog c;
  c.add(spokeDef("a", Category::Production, "ta"));  // only a "ta" connector
  c.add(spokeDef("b", Category::Storage, "tb"));     // only a "tb" connector — incompatible
  Station empty;

  const AutoLayoutResult r = autoLayout(empty, QuantityList{{"a", 1}, {"b", 1}}, c);

  CHECK(r.report.requested == 2);
  CHECK(r.report.snapped == 0);   // b can't mate a
  CHECK(r.report.floating == 2);  // a (seed) + b (overflow)
  CHECK(r.station.size() == 2);
  CHECK(noNonAdjacentOverlap(r.station, c));
}

TEST_CASE("autoLayout is additive: existing transforms unchanged; target only gains a link") {
  ModuleCatalog c;
  c.add(hubDef("hub", Category::Production));
  c.add(spokeDef("spoke", Category::Production));
  Station existing;
  PlacedModule h;
  h.defId = "hub";
  h.worldTransform.position = {5, 0, 0};
  const InstanceId hid = existing.add(h);

  const AutoLayoutResult r = autoLayout(existing, QuantityList{{"spoke", 1}}, c);

  REQUIRE(r.station.find(hid) != nullptr);
  CHECK(r.station.find(hid)->worldTransform.position.x == doctest::Approx(5.0));  // not moved
  CHECK(r.station.size() == 2);
  REQUIRE(r.placements.size() == 1);
  CHECK(r.report.snapped == 1);
  // The hub gained exactly one incoming link, to the newly-added spoke.
  REQUIRE(r.station.find(hid)->links.size() == 1);
  CHECK(r.station.find(hid)->links[0].otherInstanceId == r.placements[0].instanceId);
}

TEST_CASE("autoLayout skips unknown / non-buildable defs but places the rest") {
  ModuleCatalog c;
  c.add(hubDef("hub", Category::Production));
  ModuleDef nb = spokeDef("nb", Category::Production);
  nb.playerBuildable = false;
  c.add(nb);
  Station empty;

  const AutoLayoutResult r =
      autoLayout(empty, QuantityList{{"hub", 1}, {"ghost", 2}, {"nb", 1}}, c);

  CHECK(r.report.requested == 4);
  CHECK(r.report.skipped == 3);  // 2 unknown + 1 non-buildable
  CHECK(r.station.size() == 1);  // only the hub
  CHECK(std::find(r.report.skippedDefs.begin(), r.report.skippedDefs.end(), "ghost") !=
        r.report.skippedDefs.end());
}

namespace {
// A dock: one untyped mount connector at its origin and a clearance corridor extending
// 5 units along local +Z (half-extent 5, centred 5 out), so a mounted dock's corridor
// must point into open space to place legally.
ModuleDef dockDef(const std::string& id) {
  ModuleDef d = spokeDef(id, Category::Dock);  // category Dock, one connector "s"
  ClearanceVolume cv;
  cv.center = {0, 0, 5};
  cv.rotation = Quat{};
  cv.halfExtents = {0.4, 0.4, 5};
  cv.shipSize = "dock_s";
  d.clearanceVolumes.push_back(cv);
  return d;
}
}  // namespace

TEST_CASE("docks are placed last with a clear flight corridor") {
  ModuleCatalog c;
  c.add(hubDef("hub", Category::Production));
  c.add(spokeDef("spoke", Category::Production));
  c.add(dockDef("dock"));
  Station empty;

  const AutoLayoutResult r =
      autoLayout(empty, QuantityList{{"hub", 1}, {"spoke", 2}, {"dock", 1}}, c);

  CHECK(r.station.size() == 4);
  // The dock is ordered last, so it is the final placement.
  REQUIRE(!r.placements.empty());
  const LayoutPlacement& last = r.placements.back();
  CHECK(last.defId == "dock");

  // Its corridor is clear of EVERY other body, including its mount partner — the engine
  // enforces that a dock's flight corridor must not pierce the module it attaches to
  // (collidesClearance no longer exempts the snap partner). Ignore only the dock itself.
  const ModuleDef* dd = c.find("dock");
  REQUIRE(dd != nullptr);
  CHECK_FALSE(collidesClearance(*dd, last.worldTransform, last.instanceId, 0, r.station, c));

  // Loose "outer hull" check (spec §6: vs the median BODY module): the dock is at least as
  // far from the BODY centroid as the median body module. The dock is excluded from the
  // centroid so its own outward offset cannot drag the reference point toward itself.
  Vec3 bodyCentroid{};
  int bodyCount = 0;
  for (const auto& m : r.station.modules())
    if (m.defId != "dock") {
      bodyCentroid = bodyCentroid + m.worldTransform.position;
      ++bodyCount;
    }
  bodyCentroid = bodyCentroid * (1.0 / static_cast<double>(bodyCount));
  std::vector<double> bodyDist;
  double dockDist = 0.0;
  for (const auto& m : r.station.modules()) {
    const double dlen = length(m.worldTransform.position - bodyCentroid);
    if (m.defId == "dock")
      dockDist = dlen;
    else
      bodyDist.push_back(dlen);
  }
  std::sort(bodyDist.begin(), bodyDist.end());
  const double median = bodyDist[bodyDist.size() / 2];
  CHECK(dockDist >= median);
}

TEST_CASE("autoLayout is deterministic") {
  ModuleCatalog c;
  c.add(hubDef("hub", Category::Production));
  c.add(spokeDef("spoke", Category::Production));
  Station empty;
  const QuantityList cart{{"hub", 1}, {"spoke", 3}};

  const AutoLayoutResult r1 = autoLayout(empty, cart, c);
  const AutoLayoutResult r2 = autoLayout(empty, cart, c);

  REQUIRE(r1.placements.size() == r2.placements.size());
  for (std::size_t i = 0; i < r1.placements.size(); ++i) {
    CHECK(r1.placements[i].defId == r2.placements[i].defId);
    CHECK(r1.placements[i].worldTransform.position.x ==
          doctest::Approx(r2.placements[i].worldTransform.position.x));
    CHECK(r1.placements[i].worldTransform.position.y ==
          doctest::Approx(r2.placements[i].worldTransform.position.y));
    CHECK(r1.placements[i].worldTransform.position.z ==
          doctest::Approx(r2.placements[i].worldTransform.position.z));
  }
}

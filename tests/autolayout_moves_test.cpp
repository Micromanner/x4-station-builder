#include "x4sb/autolayout/moves.hpp"
#include "x4sb/data/math.hpp"

#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

using namespace x4sb;

namespace {
const AABB kUnit{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};

// A hub with four free ports on +/-X, +/-Z, 2 units out, untyped "snap".
ModuleDef hubDef(const std::string& id) {
  ModuleDef d;
  d.id = id;
  d.category = Category::Connector;
  d.aabb = kUnit;
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
// A spoke with one connector at its origin.
ModuleDef spokeDef(const std::string& id) {
  ModuleDef d;
  d.id = id;
  d.category = Category::Production;
  d.aabb = kUnit;
  ConnectionPoint cp;
  cp.id = "s";
  cp.localPosition = {0, 0, 0};
  cp.type = "snap";
  d.connectionPoints.push_back(cp);
  return d;
}

// hub(id1) at origin with one leaf spoke(id2) snapped to port c0; ports c1/c2/c3 free.
Station hubWithOneLeaf([[maybe_unused]] const ModuleCatalog& c) {
  Station s;
  PlacedModule h;
  h.defId = "hub";
  const InstanceId hid = s.add(h);  // id 1
  PlacedModule leaf;
  leaf.defId = "spoke";
  leaf.worldTransform.position = {2, 0, 0};  // mated on c0
  leaf.links.push_back(Link{"s", hid, "c0"});
  const InstanceId lid = s.add(leaf);  // id 2
  s.find(hid)->links.push_back(Link{"c0", lid, "s"});
  return s;
}
}  // namespace

TEST_CASE("rehostMove moves a leaf onto a different free port; structurally consistent") {
  ModuleCatalog c;
  c.add(hubDef("hub"));
  c.add(spokeDef("spoke"));
  const Station s = hubWithOneLeaf(c);
  std::mt19937 rng(123);

  const auto out = rehostMove(s, c, rng, /*minMovableId=*/1);
  REQUIRE(out.has_value());
  CHECK(out->size() == 2);
  // The leaf (id 2) still has exactly one link, to the hub (id 1), but on a DIFFERENT port.
  const PlacedModule* leaf = out->find(2);
  REQUIRE(leaf != nullptr);
  REQUIRE(leaf->links.size() == 1);
  CHECK(leaf->links[0].otherInstanceId == 1);
  CHECK(leaf->links[0].otherPointId != "c0");  // re-hosted off the original port
  // The hub's reciprocal matches (same port, points back to the leaf).
  const PlacedModule* hub = out->find(1);
  REQUIRE(hub != nullptr);
  const auto& hl = hub->links;
  CHECK(std::any_of(hl.begin(), hl.end(), [&](const Link& l) {
    return l.otherInstanceId == 2 && l.thisPointId == leaf->links[0].otherPointId;
  }));
  // No leftover reciprocal on the old port c0.
  CHECK_FALSE(
      std::any_of(hl.begin(), hl.end(), [](const Link& l) { return l.thisPointId == "c0"; }));
}

TEST_CASE("rehostMove respects minMovableId: an existing (low-id) module is never moved") {
  ModuleCatalog c;
  c.add(hubDef("hub"));
  c.add(spokeDef("spoke"));
  const Station s = hubWithOneLeaf(c);  // ids 1 (hub) and 2 (leaf)
  std::mt19937 rng(1);
  // Nothing is movable at minMovableId = 3 (no module has id >= 3).
  CHECK_FALSE(rehostMove(s, c, rng, /*minMovableId=*/3).has_value());
}

TEST_CASE("rehostMove is deterministic for a given rng state") {
  ModuleCatalog c;
  c.add(hubDef("hub"));
  c.add(spokeDef("spoke"));
  const Station s = hubWithOneLeaf(c);
  std::mt19937 a(7);
  std::mt19937 b(7);
  const auto ra = rehostMove(s, c, a, 1);
  const auto rb = rehostMove(s, c, b, 1);
  REQUIRE(ra.has_value());
  REQUIRE(rb.has_value());
  CHECK(ra->find(2)->links[0].otherPointId == rb->find(2)->links[0].otherPointId);
}

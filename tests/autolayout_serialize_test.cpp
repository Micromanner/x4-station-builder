#include "x4sb/autolayout/serialize.hpp"
#include "x4sb/data/math.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace x4sb;

namespace {
PlacedModule mod(const std::string& defId, Vec3 pos) {
  PlacedModule m;
  m.defId = defId;
  m.worldTransform.position = pos;
  return m;
}
}  // namespace

TEST_CASE("extractPlacements: greenfield emits root floating + children snapped, ascending id") {
  ModuleCatalog c;  // unused by extraction (it reads links), but kept in the signature
  Station empty;
  Station work;
  const InstanceId hub = work.add(mod("hub", {0, 0, 0}));  // id 1, root (no links)
  PlacedModule a = mod("spoke", {2, 0, 0});
  a.links.push_back(Link{"s", hub, "c0"});
  const InstanceId aid = work.add(a);  // id 2
  work.find(hub)->links.push_back(Link{"c0", aid, "s"});

  const auto p = extractPlacements(work, empty, c);
  REQUIRE(p.size() == 2);
  CHECK(p[0].instanceId == hub);
  CHECK_FALSE(p[0].snapped);  // root floats
  CHECK(p[1].instanceId == aid);
  CHECK(p[1].snapped);
  CHECK(p[1].targetInstanceId == hub);
  CHECK(p[1].newPointId == "s");
  CHECK(p[1].targetPointId == "c0");
}

TEST_CASE("extractPlacements: additive emits only modules absent from existing") {
  ModuleCatalog c;
  Station existing;
  const InstanceId pre = existing.add(mod("pre", {0, 0, 0}));  // id 1

  Station work;
  work.add(mod("pre", {0, 0, 0}));  // same id 1 (existing, carried over)
  PlacedModule added = mod("spoke", {2, 0, 0});
  added.links.push_back(Link{"s", pre, "c0"});  // snaps onto the existing module
  const InstanceId addedId = work.add(added);   // id 2

  const auto p = extractPlacements(work, existing, c);
  REQUIRE(p.size() == 1);  // only the added module
  CHECK(p[0].instanceId == addedId);
  CHECK(p[0].snapped);
  CHECK(p[0].targetInstanceId == pre);  // links onto the pre-existing module
}

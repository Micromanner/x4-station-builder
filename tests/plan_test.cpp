#include "x4sb/planio/plan.hpp"

#include "x4sb/data/catalog.hpp"

#include <doctest/doctest.h>

#include <initializer_list>

using namespace x4sb;

// Synthetic plan modelled on the real X4 construction-plan schema (NOT extracted
// game data — spec §12): a 3-entry chain exercising index/macro, the
// connection + predecessor graph, positions, and both yaw and roll rotations.
namespace {
const char* kPlan = R"(<?xml version="1.0" encoding="UTF-8"?>
<plan id="test" name="round trip" description="">
  <entry index="1" macro="struct_arg_base_01_macro"/>
  <entry index="2" macro="struct_arg_cross_01_macro" connection="ConnectionSnap002">
    <predecessor index="1" connection="ConnectionSnap001"/>
    <offset>
      <position x="-321.781" y="0" z="5701.981"/>
      <rotation yaw="-3.22959"/>
    </offset>
  </entry>
  <entry index="3" macro="storage_arg_s_solid_01_macro" connection="ConnectionSnap001">
    <predecessor index="2" connection="ConnectionSnap002"/>
    <offset>
      <position x="-344.316" y="-800" z="5501.346"/>
      <rotation roll="2.76366"/>
    </offset>
  </entry>
</plan>)";

void checkSameOrientation(const Quat& a, const Quat& b) {
  for (Vec3 probe : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
    const Vec3 ra = rotate(a, probe), rb = rotate(b, probe);
    CHECK(rb.x == doctest::Approx(ra.x));
    CHECK(rb.y == doctest::Approx(ra.y));
    CHECK(rb.z == doctest::Approx(ra.z));
  }
}
}  // namespace

TEST_CASE("importPlanXml parses the real schema: entries, links, offsets") {
  const auto st = importPlanXml(kPlan);
  REQUIRE(st.has_value());
  REQUIRE(st->size() == 3);

  const PlacedModule* m2 = st->find(2);
  REQUIRE(m2 != nullptr);
  CHECK(m2->defId == "struct_arg_cross_01_macro");
  CHECK(m2->worldTransform.position.x == doctest::Approx(-321.781));
  CHECK(m2->worldTransform.position.z == doctest::Approx(5701.981));
  REQUIRE(m2->links.size() == 1);
  CHECK(m2->links[0].thisPointId == "ConnectionSnap002");
  CHECK(m2->links[0].otherInstanceId == 1);
  CHECK(m2->links[0].otherPointId == "ConnectionSnap001");

  // First entry has no offset/predecessor -> identity transform, no links.
  const PlacedModule* m1 = st->find(1);
  REQUIRE(m1 != nullptr);
  CHECK(m1->links.empty());
}

TEST_CASE("importPlanXml also accepts a <plans> aggregate (in-game save format)") {
  const std::string aggregate = std::string("<plans>") + kPlan + "</plans>";
  const auto st = importPlanXml(aggregate);
  REQUIRE(st.has_value());
  CHECK(st->size() == 3);  // first plan extracted
}

TEST_CASE("plan survives an import -> export -> import round-trip") {
  const ModuleCatalog cat;  // export needs no catalog content yet
  const auto a = importPlanXml(kPlan);
  REQUIRE(a.has_value());

  const std::string xml = exportPlanXml(*a, cat);
  const auto b = importPlanXml(xml);
  REQUIRE(b.has_value());
  REQUIRE(a->size() == b->size());

  for (const auto& ma : a->modules()) {
    const PlacedModule* mb = b->find(ma.instanceId);
    REQUIRE(mb != nullptr);
    CHECK(mb->defId == ma.defId);
    CHECK(mb->worldTransform.position.x == doctest::Approx(ma.worldTransform.position.x));
    CHECK(mb->worldTransform.position.y == doctest::Approx(ma.worldTransform.position.y));
    CHECK(mb->worldTransform.position.z == doctest::Approx(ma.worldTransform.position.z));
    checkSameOrientation(ma.worldTransform.rotation, mb->worldTransform.rotation);
    REQUIRE(mb->links.size() == ma.links.size());
    if (!ma.links.empty()) {
      CHECK(mb->links[0].thisPointId == ma.links[0].thisPointId);
      CHECK(mb->links[0].otherInstanceId == ma.links[0].otherInstanceId);
      CHECK(mb->links[0].otherPointId == ma.links[0].otherPointId);
    }
  }
}

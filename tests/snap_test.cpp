#include "x4sb/snap/snap.hpp"

#include "x4sb/document/commands.hpp"
#include "x4sb/snap/pick.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <memory>

using namespace x4sb;

namespace {
ModuleDef makeModule(const std::string& id, const std::string& pointId, Vec3 pointPos,
                     Quat pointRot = Quat{}) {
  ModuleDef d;
  d.id = id;
  ConnectionPoint cp;
  cp.id = pointId;
  cp.localPosition = pointPos;
  cp.localRotation = pointRot;
  d.connectionPoints.push_back(cp);
  return d;
}

ModuleDef makeModule(const std::string& id, Category category, const std::string& pointId,
                     Vec3 pointPos) {
  ModuleDef d = makeModule(id, pointId, pointPos);
  d.category = category;
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

TEST_CASE("collidesWithStation accounts for rotation") {
  ModuleCatalog catalog;
  ModuleDef longDef;
  longDef.id = "L";
  longDef.aabb = AABB{{-5, -0.5, -0.5}, {5, 0.5, 0.5}};  // long along X
  catalog.add(longDef);

  Station station;
  PlacedModule a;
  a.defId = "L";  // at origin, identity -> occupies x[-5,5]
  station.add(a);

  // Same long module rotated 90deg about Z at y=3: its hull now spans y[-2,8],
  // which overlaps A. The translate-only baseline would miss this.
  const double s = std::sqrt(2.0) / 2.0;
  Transform xf;
  xf.rotation = Quat{s, 0, 0, s};
  xf.position = {0, 3, 0};
  CHECK(collidesWithStation(longDef, xf, /*ignore=*/0, station, catalog));

  // Far away -> clear.
  Transform farXf;
  farXf.rotation = Quat{s, 0, 0, s};
  farXf.position = {0, 100, 0};
  CHECK_FALSE(collidesWithStation(longDef, farXf, /*ignore=*/0, station, catalog));
}

TEST_CASE("computeSnapTransform makes the connectors face opposite directions") {
  ModuleCatalog catalog;
  const ModuleDef a = makeModule("A", "a1", {1, 0, 0});
  const ModuleDef b = makeModule("B", "b1", {-1, 0, 0});
  catalog.add(a);
  catalog.add(b);

  Station station;
  PlacedModule pa;
  pa.defId = "A";
  const InstanceId idA = station.add(pa);  // identity world transform

  const Transform xf = computeSnapTransform(station, catalog, idA, "a1", b, "b1");

  // Connector normal = local +Z (both connectionPoints have identity localRotation).
  const Vec3 axis{0, 0, 1};
  const Vec3 newNormal = rotate(xf.rotation, axis);
  CHECK(newNormal.x == doctest::Approx(0));
  CHECK(newNormal.y == doctest::Approx(0));
  CHECK(newNormal.z == doctest::Approx(-1));  // opposes the target's +Z
}

TEST_CASE("computeSnapTransform opposes normals and coincides for a rotated target") {
  ModuleCatalog catalog;
  const ModuleDef a = makeModule("A", "a1", {1, 0, 0});
  const ModuleDef b = makeModule("B", "b1", {-1, 0, 0});
  catalog.add(a);
  catalog.add(b);

  Station station;
  PlacedModule pa;
  pa.defId = "A";
  const double s = std::sqrt(2.0) / 2.0;
  pa.worldTransform.rotation = Quat{s, 0, 0, s};  // A rotated 90deg about Z
  pa.worldTransform.position = {4, 5, 6};
  const InstanceId idA = station.add(pa);

  const Transform xf = computeSnapTransform(station, catalog, idA, "a1", b, "b1");

  // Connector points coincide in world.
  const Vec3 tWorld = apply(station.find(idA)->worldTransform, Vec3{1, 0, 0});
  const Vec3 nWorld = apply(xf, Vec3{-1, 0, 0});
  CHECK(nWorld.x == doctest::Approx(tWorld.x));
  CHECK(nWorld.y == doctest::Approx(tWorld.y));
  CHECK(nWorld.z == doctest::Approx(tWorld.z));

  // Normals oppose, relationally (no hand-computed numbers).
  const Vec3 axis{0, 0, 1};
  const Vec3 tNormal = rotate(station.find(idA)->worldTransform.rotation, axis);
  const Vec3 nNormal = rotate(xf.rotation, axis);
  CHECK(nNormal.x == doctest::Approx(-tNormal.x));
  CHECK(nNormal.y == doctest::Approx(-tNormal.y));
  CHECK(nNormal.z == doctest::Approx(-tNormal.z));
}

TEST_CASE("computeSnapTransform handles non-identity connector rotations") {
  // Non-identity connector localRotations exercise the conjugate(np.localRotation)
  // term, which collapses to identity when the connectors are unrotated.
  const double s = std::sqrt(2.0) / 2.0;
  const Quat qA{s, s, 0, 0};  // 90deg about X
  const Quat qB{s, 0, s, 0};  // 90deg about Y
  const ModuleDef a = makeModule("A", "a1", {1, 0, 0}, qA);
  const ModuleDef b = makeModule("B", "b1", {-1, 0, 0}, qB);

  ModuleCatalog catalog;
  catalog.add(a);
  catalog.add(b);

  Station station;
  PlacedModule pa;
  pa.defId = "A";
  pa.worldTransform.rotation = Quat{s, 0, 0, s};  // A rotated 90deg about Z
  pa.worldTransform.position = {2, -3, 4};
  const InstanceId idA = station.add(pa);

  const Transform xf = computeSnapTransform(station, catalog, idA, "a1", b, "b1");

  // Connector points coincide in world.
  const Vec3 tWorld = apply(station.find(idA)->worldTransform, Vec3{1, 0, 0});
  const Vec3 nWorld = apply(xf, Vec3{-1, 0, 0});
  CHECK(nWorld.x == doctest::Approx(tWorld.x));
  CHECK(nWorld.y == doctest::Approx(tWorld.y));
  CHECK(nWorld.z == doctest::Approx(tWorld.z));

  // Each connector's WORLD normal = (module world rotation * connector localRotation)
  // applied to the +Z convention axis. They must oppose.
  const Vec3 axis{0, 0, 1};
  const Vec3 tNormal = rotate(station.find(idA)->worldTransform.rotation * qA, axis);
  const Vec3 nNormal = rotate(xf.rotation * qB, axis);
  CHECK(nNormal.x == doctest::Approx(-tNormal.x));
  CHECK(nNormal.y == doctest::Approx(-tNormal.y));
  CHECK(nNormal.z == doctest::Approx(-tNormal.z));
}

TEST_CASE("collidesWithStation skips the ignored instance") {
  ModuleCatalog catalog;
  ModuleDef box;
  box.id = "B";
  box.aabb = AABB{{-1, -1, -1}, {1, 1, 1}};
  catalog.add(box);

  Station station;
  PlacedModule a;
  a.defId = "B";  // at origin -> overlaps a candidate placed at origin
  const InstanceId idA = station.add(a);

  Transform here;  // identity, at origin
  CHECK(collidesWithStation(box, here, /*ignore=*/0, station, catalog));
  CHECK_FALSE(collidesWithStation(box, here, /*ignore=*/idA, station, catalog));
}

TEST_CASE("makeSnapPlacement returns an executable command for an in-range target") {
  ModuleCatalog catalog;
  catalog.add(makeModule("A", "a1", {0, 0, 0}));
  const ModuleDef bdef = makeModule("B", "b1", {0, 0, 0});
  catalog.add(bdef);

  Station station;
  PlacedModule pa;
  pa.defId = "A";
  pa.worldTransform.position = {10, 0, 0};  // connector world pos = {10,0,0}
  station.add(pa);

  std::unique_ptr<Command> cmd =
      makeSnapPlacement(bdef, Vec3{10.2, 0, 0}, station, catalog, 1.0);
  REQUIRE(cmd != nullptr);

  UndoStack stack;
  stack.execute(station, std::move(cmd));
  CHECK(station.size() == 2);

  const auto& mods = station.modules();
  const PlacedModule& newMod = mods.back();
  REQUIRE(newMod.links.size() == 1);
  CHECK(newMod.links[0].thisPointId == "b1");
  CHECK(newMod.links[0].otherPointId == "a1");
  const PlacedModule* target = station.find(mods.front().instanceId);
  REQUIRE(target->links.size() == 1);
  CHECK(target->links[0].otherInstanceId == newMod.instanceId);

  // No free connector in range -> nullptr.
  CHECK(makeSnapPlacement(bdef, Vec3{0, 0, 0}, station, catalog, 1.0) == nullptr);
}

TEST_CASE("makeSnapPlacement returns nullptr when the placement would collide") {
  ModuleCatalog catalog;
  ModuleDef adef = makeModule("A", "a1", {0, 0, 0});
  adef.aabb = AABB{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};
  catalog.add(adef);
  ModuleDef bdef = makeModule("B", "b1", {0, 0, 0});
  bdef.aabb = AABB{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};
  catalog.add(bdef);
  ModuleDef blocker;  // big hull enveloping B's landing spot; has no connectors, isn't the target
  blocker.id = "X";
  blocker.aabb = AABB{{-5, -5, -5}, {5, 5, 5}};
  catalog.add(blocker);

  Station station;
  PlacedModule pa;
  pa.defId = "A";
  pa.worldTransform.position = {10, 0, 0};
  station.add(pa);
  PlacedModule px;
  px.defId = "X";
  px.worldTransform.position = {10, 0, 0};
  station.add(px);

  // Candidate A.a1 is found in range, but B lands at ~{10,0,0} inside X's hull
  // (A is the ignored joint partner) -> collision -> nullptr.
  CHECK(makeSnapPlacement(bdef, Vec3{10.2, 0, 0}, station, catalog, 1.0) == nullptr);
}

TEST_CASE("findSnapCandidate skips the ignored instance") {
  // Two placed modules, each with one free compatible connector. A cursor near
  // module A's connector normally returns A; ignoring A must return B instead
  // (or none if B is out of range).
  ModuleCatalog c;
  ModuleDef a = makeModule("a_mod", Category::Production, "a1", {0.5, 0, 0});
  ModuleDef b = makeModule("b_mod", Category::Production, "b1", {0.5, 0, 0});
  c.add(a);
  c.add(b);

  Station s;
  PlacedModule pa;
  pa.defId = "a_mod";  // a1 world = (0.5, 0, 0)
  PlacedModule pb;
  pb.defId = "b_mod";
  pb.worldTransform.position = {10, 0, 0};  // b1 world = (10.5, 0, 0)
  const InstanceId ida = s.add(pa);
  s.add(pb);

  ModuleDef mover = makeModule("m_mod", Category::Production, "m1", {0, 0, 0});

  // Cursor right at A's connector, generous radius: without ignore -> picks A.
  const auto hit = findSnapCandidate(mover, Vec3{0.5, 0, 0}, s, c, 100.0);
  REQUIRE(hit.has_value());
  CHECK(hit->instanceId == ida);

  // Ignoring A -> must now pick B (the only other free connector in range).
  const auto hitIgnoreA = findSnapCandidate(mover, Vec3{0.5, 0, 0}, s, c, 100.0, ida);
  REQUIRE(hitIgnoreA.has_value());
  CHECK(hitIgnoreA->instanceId != ida);
}

TEST_CASE("integration: snap-place then pick then move detaches and undo reattaches") {
  ModuleCatalog catalog;
  ModuleDef adef = makeModule("A", "a1", {1, 0, 0});
  adef.aabb = AABB{{-0.4, -0.4, -0.4}, {0.4, 0.4, 0.4}};
  catalog.add(adef);
  ModuleDef bdef = makeModule("B", "b1", {-2, 0, 0});
  bdef.aabb = AABB{{-0.4, -0.4, -0.4}, {0.4, 0.4, 0.4}};
  catalog.add(bdef);

  Station station;
  UndoStack stack;
  PlacedModule pa;
  pa.defId = "A";  // origin; connector a1 world = {1,0,0}
  const InstanceId idA = station.add(pa);

  // Snap-place B onto A's connector (B lands centred at {-1,0,0}).
  std::unique_ptr<Command> place = makeSnapPlacement(bdef, Vec3{1, 0, 0}, station, catalog, 0.5);
  REQUIRE(place != nullptr);
  stack.execute(station, std::move(place));
  REQUIRE(station.size() == 2);

  InstanceId idB = 0;
  for (const auto& m : station.modules())
    if (m.instanceId != idA) idB = m.instanceId;
  REQUIRE(idB != 0);
  REQUIRE(station.find(idB)->links.size() == 1);
  REQUIRE(station.find(idA)->links.size() == 1);
  CHECK(station.find(idB)->worldTransform.position.x == doctest::Approx(-1));

  // Pick B with a ray through its position (misses A at the origin).
  const auto picked = pickModule(station, catalog, Vec3{-1, 0, -100}, Vec3{0, 0, 1});
  REQUIRE(picked.has_value());
  CHECK(*picked == idB);

  // Move the picked module away -> detaches both sides.
  Transform moved;
  moved.position = {1000, 0, 0};
  stack.execute(station, std::make_unique<MoveModuleCommand>(*picked, moved));
  CHECK(station.find(idB)->links.empty());
  CHECK(station.find(idA)->links.empty());

  // Undo the move -> reattached at the original spot.
  stack.undo(station);
  CHECK(station.find(idB)->links.size() == 1);
  CHECK(station.find(idA)->links.size() == 1);
  CHECK(station.find(idB)->worldTransform.position.x == doctest::Approx(-1));
}

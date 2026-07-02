#include "x4sb/snap/snap.hpp"

#include "x4sb/document/commands.hpp"
#include "x4sb/snap/connector_grid.hpp"
#include "x4sb/snap/pick.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

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

TEST_CASE("connectorsCompatible: auxiliary tags do not gate mating, venture does") {
  // Ground truth from real construction plans (2026-07-03): the game links "snap" ports to
  // "snap snap_aligned" (user plans: vertical structs onto crosses/piers/habitats) and to
  // "snap ter_energycells" (vanilla ter_solarpowerplant: energycells onto a plain-snap
  // dockarea). Exact-string equality wrongly rejected those, making whole module classes
  // unplaceable by auto-layout (known-issues 4.3). "venture" stays discriminating: no vanilla
  // plan mates venture to plain snap, and loosening it re-opens the venture-trap.
  const auto pt = [](const char* type) {
    ConnectionPoint cp;
    cp.type = type;
    return cp;
  };
  CHECK(connectorsCompatible(pt("snap"), pt("snap")));
  CHECK(connectorsCompatible(pt("snap snap_aligned"), pt("snap")));
  CHECK(connectorsCompatible(pt("snap ter_energycells"), pt("snap")));
  CHECK(connectorsCompatible(pt("snap snap_aligned"), pt("snap ter_energycells")));
  CHECK(connectorsCompatible(pt("snap venture"), pt("snap venture")));
  CHECK(connectorsCompatible(pt("snap venture"), pt("snap venture snap_aligned")));
  CHECK_FALSE(connectorsCompatible(pt("snap venture"), pt("snap")));
  CHECK_FALSE(connectorsCompatible(pt("snap venture snap_aligned"), pt("snap snap_aligned")));
  // Non-snap tag strings mate nothing (a snap port never pairs a non-snap connection).
  CHECK_FALSE(connectorsCompatible(pt("venture_only_nomatch"), pt("snap")));
  // The empty type stays a wildcard (synthetic fixtures; catalog ports always carry "snap").
  CHECK(connectorsCompatible(pt(""), pt("snap venture")));
}

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

TEST_CASE("collidesWithStation is OBB-precise: rotated neighbours don't falsely collide") {
  ModuleCatalog catalog;
  ModuleDef longDef;
  longDef.id = "L";
  longDef.aabb = AABB{{-5, -0.5, -0.5}, {5, 0.5, 0.5}};  // long along X
  catalog.add(longDef);

  const Quat z45 = axisAngle(Vec3{0, 0, 1}, 0.7853981633974483);  // pi/4
  Station station;
  PlacedModule a;
  a.defId = "L";
  a.worldTransform.rotation = z45;  // diagonal bar at origin
  station.add(a);

  const Vec3 perp{-std::sqrt(2.0) / 2.0, std::sqrt(2.0) / 2.0, 0};  // perp to the bar in XY

  // Parallel bar, 0.5 perpendicular gap: the sqrt(2)-inflated AABB hulls overlap (the
  // old AABB-only test rejected this and forced auto-layout to spawn another hub), but
  // the oriented bodies are clear -> OBB narrow phase must report NO collision.
  Transform clear;
  clear.rotation = z45;
  clear.position = perp * 1.5;  // perpendicular separation 1.5 > 0.5+0.5
  CHECK_FALSE(collidesWithStation(longDef, clear, /*ignore=*/0, station, catalog));

  // Slid in to a 0.5 perpendicular separation -> bodies genuinely interpenetrate.
  Transform hit;
  hit.rotation = z45;
  hit.position = perp * 0.5;
  CHECK(collidesWithStation(longDef, hit, /*ignore=*/0, station, catalog));
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

  std::unique_ptr<Command> cmd = makeSnapPlacement(bdef, Vec3{10.2, 0, 0}, station, catalog, 1.0);
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

TEST_CASE(
    "findSnapCandidate uses connector-to-connector distance when newDefTransform is provided") {
  ModuleCatalog catalog;
  catalog.add(makeModule("A", "a1", {0.5, 0, 0}));
  const ModuleDef newDef = makeModule("B", "b1", {-0.5, 0, 0});

  Station station;
  PlacedModule pa;
  pa.defId = "A";
  pa.worldTransform.position = {0, 0, 0};  // connector a1 at {0.5, 0, 0}
  const InstanceId ida = station.add(pa);

  // Dragging B with a preview transform at {2, 0, 0}.
  // The moving connector b1 is at {1.5, 0, 0}.
  // Distance between connector a1 (0.5) and b1 (1.5) is 1.0.
  Transform freePose;
  freePose.position = {2, 0, 0};

  // If using old cursor distance (from freePose.position {2,0,0}), distance to a1 (0.5) is 1.5.
  // With a small radius of 1.2:
  // Using cursor (no newDefTransform): should miss because 1.5 > 1.2
  const auto miss = findSnapCandidate(newDef, freePose.position, station, catalog, 1.2, ida + 1);
  CHECK_FALSE(miss.has_value());

  // Using newDefTransform: should hit because connector distance is 1.0 <= 1.2
  const auto hit =
      findSnapCandidate(newDef, freePose.position, station, catalog, 1.2, ida + 1, freePose);
  REQUIRE(hit.has_value());
  CHECK(hit->instanceId == ida);
  CHECK(hit->targetPointId == "a1");
  CHECK(hit->newPointId == "b1");
}

TEST_CASE("grid-backed findSnapCandidate matches brute force (cursor mode)") {
  ModuleCatalog catalog;
  catalog.add(makeModule("A", "a1", {0, 0, 0}));
  const ModuleDef newDef = makeModule("B", "b1", {0, 0, 0});  // untagged -> compatible

  Station station;
  for (int i = 0; i < 200; ++i) {
    PlacedModule pm;
    pm.defId = "A";
    pm.worldTransform.position = {static_cast<double>(i) * 137.0, 0, 0};
    station.add(pm);
  }
  const ConnectorGrid grid(station, catalog, 1000.0);

  for (double cx : {50.0, 1500.0, 9000.0, 12345.0}) {
    const Vec3 cursor{cx, 0, 0};
    const auto brute = findSnapCandidate(newDef, cursor, station, catalog, 1000.0);
    const auto fast = findSnapCandidate(newDef, cursor, station, catalog, grid, 1000.0);
    CHECK(brute.has_value() == fast.has_value());
    if (brute && fast) {
      CHECK(fast->instanceId == brute->instanceId);
      CHECK(fast->targetPointId == brute->targetPointId);
      CHECK(fast->newPointId == brute->newPointId);
    }
  }
}

TEST_CASE("grid-backed findSnapCandidate matches brute force (snap-on-move)") {
  ModuleCatalog catalog;
  catalog.add(makeModule("A", "a1", {0, 0, 0}));
  const ModuleDef newDef = makeModule("B", "b1", {5, 0, 0});

  Station station;
  for (int i = 0; i < 100; ++i) {
    PlacedModule pm;
    pm.defId = "A";
    pm.worldTransform.position = {static_cast<double>(i) * 211.0, 0, 0};
    station.add(pm);
  }
  const ConnectorGrid grid(station, catalog, 1000.0);

  for (double px : {3.0, 1000.0, 8000.0}) {
    Transform newXf;
    newXf.position = {px, 0, 0};
    const auto brute = findSnapCandidate(newDef, newXf.position, station, catalog, 300.0, 0, newXf);
    const auto fast =
        findSnapCandidate(newDef, newXf.position, station, catalog, grid, 300.0, 0, newXf);
    CHECK(brute.has_value() == fast.has_value());
    if (brute && fast) CHECK(fast->instanceId == brute->instanceId);
  }
}

TEST_CASE("grid-backed findSnapCandidate honors ignoreInstanceId") {
  ModuleCatalog catalog;
  catalog.add(makeModule("A", "a1", {0, 0, 0}));
  const ModuleDef newDef = makeModule("B", "b1", {0, 0, 0});

  Station station;
  PlacedModule pm;
  pm.defId = "A";
  pm.worldTransform.position = {500, 0, 0};
  const InstanceId id = station.add(pm);
  const ConnectorGrid grid(station, catalog, 1000.0);

  CHECK(findSnapCandidate(newDef, Vec3{500, 0, 0}, station, catalog, grid, 1000.0).has_value());
  CHECK_FALSE(
      findSnapCandidate(newDef, Vec3{500, 0, 0}, station, catalog, grid, 1000.0, id).has_value());
}

TEST_CASE("collidesClearance flags a module blocking a dock corridor") {
  ModuleCatalog catalog;

  // A dock module: small body, with a clearance corridor extending +Z to ~z=600.
  ModuleDef dock;
  dock.id = "DOCK";
  dock.aabb = AABB{{-5, -5, -5}, {5, 5, 5}};
  ClearanceVolume cv;
  cv.rotation = Quat{};  // outward = +Z
  cv.halfExtents = {50, 50, 300};
  cv.center = {0, 0, 300};  // spans z[0,600]
  cv.shipSize = "dock_l";
  dock.clearanceVolumes.push_back(cv);
  catalog.add(dock);

  // A plain blocker module.
  ModuleDef blocker;
  blocker.id = "BLOCK";
  blocker.aabb = AABB{{-10, -10, -10}, {10, 10, 10}};
  catalog.add(blocker);

  Station station;
  PlacedModule d;  // dock at origin, identity
  d.defId = "DOCK";
  station.add(d);

  // Place a blocker inside the corridor (z=300) -> clearance collision.
  Transform inCorridor;
  inCorridor.position = {0, 0, 300};
  CHECK(collidesClearance(blocker, inCorridor, 0, 0, station, catalog));

  // Place it off to the side, clear of the corridor.
  Transform aside;
  aside.position = {500, 0, 300};
  CHECK_FALSE(collidesClearance(blocker, aside, 0, 0, station, catalog));
}

TEST_CASE("violatedClearance names the dock corridors a body intrudes") {
  ModuleCatalog catalog;

  // A dock with one corridor spanning z[0,600].
  ModuleDef dock;
  dock.id = "DOCK";
  dock.aabb = AABB{{-5, -5, -5}, {5, 5, 5}};
  ClearanceVolume cv;
  cv.rotation = Quat{};  // outward = +Z
  cv.halfExtents = {50, 50, 300};
  cv.center = {0, 0, 300};
  cv.shipSize = "dock_l";
  dock.clearanceVolumes.push_back(cv);
  catalog.add(dock);

  ModuleDef blocker;
  blocker.id = "BLOCK";
  blocker.aabb = AABB{{-10, -10, -10}, {10, 10, 10}};
  catalog.add(blocker);

  Station station;
  PlacedModule d;
  d.defId = "DOCK";
  const InstanceId dockId = station.add(d);

  // A body inside the corridor (z=300) -> one hit naming the dock's volume 0.
  Transform inCorridor;
  inCorridor.position = {0, 0, 300};
  const std::vector<ClearanceHit> hits =
      violatedClearance(blocker, inCorridor, 0, 0, station, catalog);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].blocker == dockId);
  CHECK(hits[0].volumeIndex == 0);

  // Off to the side -> no hits.
  Transform aside;
  aside.position = {500, 0, 300};
  CHECK(violatedClearance(blocker, aside, 0, 0, station, catalog).empty());

  // The dock never reports blocking its own corridor (ignored id).
  CHECK(violatedClearance(dock, Transform{}, dockId, 0, station, catalog).empty());
}

namespace {
// Real Advanced Composite Production geometry (two 90°-apart snap connectors).
ModuleDef advComposite() {
  ModuleDef d;
  d.id = "adv";
  d.aabb = AABB{{-400.14731, -799.99985, -180.89929}, {379.82936, 683.47575, 183.54531}};
  ConnectionPoint c1;
  c1.id = "ConnectionSnap001";
  c1.localPosition = {200.0, -800.0, 0.0};
  c1.localRotation = {-0.7071067, -0.7071068, 0.0, 0.0};
  c1.type = "snap";
  ConnectionPoint c2;
  c2.id = "ConnectionSnap002";
  c2.localPosition = {-400.0, 600.0, 0.0};
  c2.localRotation = {-0.7071069, -5.024295e-15, 0.7071066, -6.181723e-08};
  c2.type = "snap";
  d.connectionPoints = {c1, c2};
  return d;
}
constexpr double kQuarterTurn = 1.5707963267948966;  // pi/2
}  // namespace

TEST_CASE("computeSnapTransform clockingRad=0 equals the no-arg baseline") {
  ModuleCatalog catalog;
  catalog.add(makeModule("A", "a1", {1, 0, 0}));
  const ModuleDef b = makeModule("B", "b1", {-1, 0, 0});
  catalog.add(b);
  Station station;
  PlacedModule pa;
  pa.defId = "A";
  const InstanceId idA = station.add(pa);

  const Transform base = computeSnapTransform(station, catalog, idA, "a1", b, "b1");
  const Transform zero = computeSnapTransform(station, catalog, idA, "a1", b, "b1", 0.0);
  CHECK(zero.position.x == doctest::Approx(base.position.x));
  CHECK(zero.position.y == doctest::Approx(base.position.y));
  CHECK(zero.position.z == doctest::Approx(base.position.z));
  CHECK(std::abs(zero.rotation.w * base.rotation.w + zero.rotation.x * base.rotation.x +
                 zero.rotation.y * base.rotation.y + zero.rotation.z * base.rotation.z) ==
        doctest::Approx(1.0));
}

TEST_CASE("clocking rolls about the connector axis but keeps the joint coincident") {
  ModuleCatalog catalog;
  const ModuleDef adv = advComposite();
  catalog.add(adv);
  Station station;
  PlacedModule p0;
  p0.defId = "adv";
  const InstanceId id0 = station.add(p0);

  const ConnectionPoint& host = adv.connectionPoints[0];  // ConnectionSnap001
  const ConnectionPoint& newc = adv.connectionPoints[1];  // ConnectionSnap002
  const Vec3 hostWorld = apply(station.find(id0)->worldTransform, host.localPosition);

  for (const double clk : {0.0, kQuarterTurn, -kQuarterTurn}) {
    const Transform xf = computeSnapTransform(station, catalog, id0, host.id, adv, newc.id, clk);
    const Vec3 newWorld = apply(xf, newc.localPosition);
    CHECK(newWorld.x == doctest::Approx(hostWorld.x));  // joint stays put under clocking
    CHECK(newWorld.y == doctest::Approx(hostWorld.y));
    CHECK(newWorld.z == doctest::Approx(hostWorld.z));
  }
}

TEST_CASE("clocking -pi/2 closes a flat 4-module ring; clocking 0 worms") {
  ModuleCatalog catalog;
  const ModuleDef adv = advComposite();
  catalog.add(adv);

  const auto buildChain = [&](double clk) {
    Station st;
    PlacedModule m0;
    m0.defId = "adv";
    InstanceId prev = st.add(m0);
    std::vector<InstanceId> ids{prev};
    for (int i = 0; i < 3; ++i) {
      const Transform xf = computeSnapTransform(st, catalog, prev, "ConnectionSnap001", adv,
                                                "ConnectionSnap002", clk);
      PlacedModule m;
      m.defId = "adv";
      m.worldTransform = xf;
      prev = st.add(m);
      ids.push_back(prev);
    }
    return std::pair<Station, std::vector<InstanceId>>{st, ids};
  };

  auto [flat, flatIds] = buildChain(-kQuarterTurn);
  const Vec3 openA =
      apply(flat.find(flatIds.front())->worldTransform, Vec3{-400, 600, 0});  // m0 C002
  const Vec3 openB =
      apply(flat.find(flatIds.back())->worldTransform, Vec3{200, -800, 0});  // m3 C001
  CHECK(length(openA - openB) < 1.0);                                        // ring closes
  double zMin = 1e30, zMax = -1e30;
  for (const InstanceId id : flatIds) {
    const double z = flat.find(id)->worldTransform.position.z;
    zMin = std::min(zMin, z);
    zMax = std::max(zMax, z);
  }
  CHECK(zMax - zMin < 1.0);  // planar

  auto [worm, wormIds] = buildChain(0.0);
  const Vec3 wA = apply(worm.find(wormIds.front())->worldTransform, Vec3{-400, 600, 0});
  const Vec3 wB = apply(worm.find(wormIds.back())->worldTransform, Vec3{200, -800, 0});
  CHECK(length(wA - wB) > 1000.0);  // does not close — the 3D worm
}

TEST_CASE("chooseMate recovers the clocking that reproduces a desired orientation") {
  ModuleCatalog catalog;
  const ModuleDef adv = advComposite();
  catalog.add(adv);
  Station station;
  PlacedModule p0;
  p0.defId = "adv";
  const InstanceId id0 = station.add(p0);

  // The flat-ring mate: host C001, new C002, clocking -pi/2. Use its orientation as
  // the user's "desired" rotation and confirm chooseMate recovers connector + clocking.
  const Transform flat = computeSnapTransform(station, catalog, id0, "ConnectionSnap001", adv,
                                              "ConnectionSnap002", -kQuarterTurn);
  const auto choice = chooseMate(station, catalog, id0, "ConnectionSnap001", adv, flat.rotation);
  REQUIRE(choice.has_value());
  CHECK(choice->newPointId == "ConnectionSnap002");
  const Quat& r = choice->xf.rotation;
  CHECK(std::abs(r.w * flat.rotation.w + r.x * flat.rotation.x + r.y * flat.rotation.y +
                 r.z * flat.rotation.z) == doctest::Approx(1.0));
}

TEST_CASE("chooseMate breaks ties toward the lower connector index") {
  // Two identical connectors -> identical error for any desired rotation -> first wins.
  ModuleDef d;
  d.id = "sym";
  ConnectionPoint a;
  a.id = "first";
  a.localPosition = {1, 0, 0};
  ConnectionPoint b;
  b.id = "second";
  b.localPosition = {1, 0, 0};
  d.connectionPoints = {a, b};
  ModuleCatalog catalog;
  catalog.add(makeModule("H", "h1", {0, 0, 0}));
  catalog.add(d);
  Station station;
  PlacedModule ph;
  ph.defId = "H";
  const InstanceId idH = station.add(ph);

  const auto choice = chooseMate(station, catalog, idH, "h1", d, Quat{});
  REQUIRE(choice.has_value());
  CHECK(choice->newPointId == "first");
}

TEST_CASE("chooseMate returns nullopt when nothing is compatible") {
  ModuleCatalog catalog;
  ModuleDef host = makeModule("H", "h1", {0, 0, 0});
  host.connectionPoints[0].type = "tagA";
  catalog.add(host);
  ModuleDef other = makeModule("O", "o1", {0, 0, 0});
  other.connectionPoints[0].type = "tagB";  // incompatible
  catalog.add(other);
  Station station;
  PlacedModule ph;
  ph.defId = "H";
  const InstanceId idH = station.add(ph);
  CHECK_FALSE(chooseMate(station, catalog, idH, "h1", other, Quat{}).has_value());
}

TEST_CASE("collidesWithStation two-id overload skips both instances") {
  ModuleCatalog catalog;
  ModuleDef box;
  box.id = "B";
  box.aabb = AABB{{-1, -1, -1}, {1, 1, 1}};
  catalog.add(box);

  Station station;
  PlacedModule p0;  // id 1, at origin
  p0.defId = "B";
  const InstanceId id0 = station.add(p0);
  PlacedModule p1;  // id 2, touching at x=2
  p1.defId = "B";
  p1.worldTransform.position = {2, 0, 0};
  const InstanceId id1 = station.add(p1);

  Transform at0;  // overlaps both id0 (origin) and the touching id1
  CHECK(collidesWithStation(box, at0, 0, 0, station, catalog));            // nothing ignored
  CHECK(collidesWithStation(box, at0, id0, 0, station, catalog));          // still hits id1 (touch)
  CHECK_FALSE(collidesWithStation(box, at0, id0, id1, station, catalog));  // both ignored
}

TEST_CASE("rollAboutConnector: a single-joint module pivots about the connector for ANY axis") {
  Transform start;  // identity at origin; its connector local {1,0,0} sits at the joint
  start.position = {0, 0, 0};
  const Vec3 cpPos{1, 0, 0};  // connector world position (the joint)
  // An OFF-axis rotation (about Y, not the connector axis) still pivots about the joint and
  // keeps the module mated — the connector point does not move; the body hinges around it.
  const RollResult r = rollAboutConnector(start, Vec3{0, 1, 0}, 1.5707963267948966, cpPos, true);
  CHECK(r.mated);
  const Vec3 jointAfter = r.preview.position + rotate(r.preview.rotation, Vec3{1, 0, 0});
  CHECK(jointAfter.x == doctest::Approx(1));
  CHECK(jointAfter.y == doctest::Approx(0));
  CHECK(jointAfter.z == doctest::Approx(0));
}

TEST_CASE("rollAboutConnector: 0 or 2+ joints free-rotate in place, mated=false") {
  Transform start;
  start.position = {5, 0, 0};
  const RollResult none = rollAboutConnector(start, Vec3{0, 0, 1}, 1.0, Vec3{6, 0, 0}, false);
  CHECK_FALSE(none.mated);
  CHECK(none.preview.position.x == doctest::Approx(5));  // position unchanged (in place)
  CHECK(none.preview.position.y == doctest::Approx(0));
  CHECK(none.preview.position.z == doctest::Approx(0));
}

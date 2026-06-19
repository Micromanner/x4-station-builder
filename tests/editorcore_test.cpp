#include "x4sb/editorcore/editor_state.hpp"

#include "x4sb/data/catalog.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

namespace {
// Build a module with a single connection point (mirrors snap_test's helper).
ModuleDef makeModule(const std::string& id, Category cat, const std::string& pointId,
                     Vec3 pointPos, const std::string& type = "") {
  ModuleDef d;
  d.id = id;
  d.category = cat;
  d.aabb = AABB{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};
  ConnectionPoint cp;
  cp.id = pointId;
  cp.localPosition = pointPos;
  cp.type = type;
  d.connectionPoints.push_back(cp);
  return d;
}

ModuleCatalog twoModuleCatalog() {
  ModuleCatalog c;
  c.add(makeModule("a_mod", Category::Production, "a1", {0.5, 0, 0}));
  c.add(makeModule("b_mod", Category::Storage, "b1", {-0.5, 0, 0}));
  return c;
}
}  // namespace

TEST_CASE("active module: stable order, cycle wraps, filter narrows") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);

  // Order is sorted by id: a_mod (0), b_mod (1).
  REQUIRE(s.activeCount() == 2);
  REQUIRE(s.activeDef() != nullptr);
  CHECK(s.activeDef()->id == "a_mod");

  s.cycleActive(1);
  CHECK(s.activeDef()->id == "b_mod");
  s.cycleActive(1);  // wraps
  CHECK(s.activeDef()->id == "a_mod");
  s.cycleActive(-1);  // wraps backward
  CHECK(s.activeDef()->id == "b_mod");

  s.setFilter(Category::Storage);
  CHECK(s.activeCount() == 1);
  REQUIRE(s.activeDef() != nullptr);
  CHECK(s.activeDef()->id == "b_mod");

  s.setFilter(Category::Defense);  // empty category
  CHECK(s.activeCount() == 0);
  CHECK(s.activeDef() == nullptr);

  s.setFilter(std::nullopt);  // clear
  CHECK(s.activeCount() == 2);
}

TEST_CASE("root free-place: ghost on ground, commit adds module, undo/redo") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);  // active = a_mod

  // Ray from above pointing straight down hits ground plane y=0 at (3,0,-7).
  s.updateGhost(/*origin=*/Vec3{3, 10, -7}, /*dir=*/Vec3{0, -1, 0});
  REQUIRE(s.ghost().has_value());
  CHECK(s.ghost()->valid);
  CHECK_FALSE(s.ghost()->candidate.has_value());  // root: no snap target
  CHECK(s.ghost()->worldTransform.position.x == doctest::Approx(3));
  CHECK(s.ghost()->worldTransform.position.y == doctest::Approx(0));
  CHECK(s.ghost()->worldTransform.position.z == doctest::Approx(-7));

  const std::optional<InstanceId> id = s.commitGhost();
  REQUIRE(id.has_value());
  CHECK(s.station().size() == 1);
  CHECK(s.canUndo());
  CHECK_FALSE(s.ghost().has_value());  // consumed on commit

  s.undo();
  CHECK(s.station().size() == 0);
  CHECK(s.canRedo());
  s.redo();
  CHECK(s.station().size() == 1);
}

TEST_CASE("free-place uses a camera standoff, not the ground plane") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.setPlaceDistance(20.0);
  // An up-pointing ray used to mean "no ground hit -> no ghost"; with a view-facing
  // standoff the ghost simply sits placeDistance along the ray, so vertical aim
  // still places (and moving the mouse up/down moves the ghost up/down on screen).
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, 1, 0});
  REQUIRE(s.ghost().has_value());
  CHECK_FALSE(s.ghost()->candidate.has_value());
  CHECK(s.ghost()->worldTransform.position.x == doctest::Approx(0));
  CHECK(s.ghost()->worldTransform.position.y == doctest::Approx(30));  // 10 + 1*20
  CHECK(s.ghost()->worldTransform.position.z == doctest::Approx(0));
}

TEST_CASE("snap ghost: aim at a placed module -> previews onto its free connector") {
  const ModuleCatalog c = twoModuleCatalog();  // a_mod conn a1 at (+0.5,0,0); b_mod conn b1
  EditorState s(c);  // active = a_mod

  // Free-place a_mod as the root at the origin (ray down at (0,0,0)).
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  REQUIRE(s.commitGhost().has_value());
  REQUIRE(s.station().size() == 1);

  // Switch active module to b_mod, then aim a ray at the placed a_mod's box.
  s.cycleActive(1);
  REQUIRE(s.activeDef()->id == "b_mod");
  s.updateGhost(/*origin=*/Vec3{0, 0, -10}, /*dir=*/Vec3{0, 0, 1});  // hits a_mod at origin

  REQUIRE(s.ghost().has_value());
  REQUIRE(s.ghost()->candidate.has_value());          // snapped, not root
  CHECK(s.ghost()->candidate->targetPointId == "a1");
  CHECK(s.ghost()->candidate->newPointId == "b1");
  CHECK(s.ghost()->defId == "b_mod");

  // Commit links the two modules reciprocally.
  REQUIRE(s.commitGhost().has_value());
  REQUIRE(s.station().size() == 2);
  const auto& mods = s.station().modules();
  CHECK(mods.back().links.size() == 1);
  CHECK(mods.back().links[0].thisPointId == "b1");
  CHECK(mods.back().links[0].otherPointId == "a1");
}

TEST_CASE("free-place ghost when the ray hits no module") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});  // root
  REQUIRE(s.commitGhost().has_value());
  s.cycleActive(1);
  // Aiming at empty space no longer means "no ghost" — it free-places at the
  // standoff, which is what lets you start a disconnected second cluster.
  s.updateGhost(Vec3{100, 100, 100}, Vec3{0, 1, 0});
  REQUIRE(s.ghost().has_value());
  CHECK_FALSE(s.ghost()->candidate.has_value());
}

TEST_CASE("togglePlacement: select mode suppresses the ghost so clicks can select") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);  // build mode by default, active = a_mod
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  REQUIRE(s.ghost().has_value());  // build mode: a ghost is present

  s.togglePlacement();  // -> select mode
  CHECK_FALSE(s.placementEnabled());
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  CHECK_FALSE(s.ghost().has_value());          // no ghost -> a left-click will select
  CHECK(s.commitGhost() == std::nullopt);      // nothing to commit in select mode

  s.togglePlacement();  // -> back to build mode
  CHECK(s.placementEnabled());
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  CHECK(s.ghost().has_value());
}

TEST_CASE("select by ray, delete, and undo restores the module") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);

  // Place a_mod at the origin.
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  const InstanceId placed = s.commitGhost().value();

  // Ray straight at the origin selects it.
  const std::optional<InstanceId> sel = s.selectByRay(Vec3{0, 0, -10}, Vec3{0, 0, 1});
  REQUIRE(sel.has_value());
  CHECK(*sel == placed);
  CHECK(s.selected().value() == placed);

  // A ray into empty space clears the selection.
  CHECK_FALSE(s.selectByRay(Vec3{50, 50, 50}, Vec3{0, 1, 0}).has_value());
  CHECK_FALSE(s.selected().has_value());

  // Re-select and delete.
  s.selectByRay(Vec3{0, 0, -10}, Vec3{0, 0, 1});
  CHECK(s.deleteSelected());
  CHECK(s.station().size() == 0);
  CHECK_FALSE(s.selected().has_value());  // selection cleared on delete

  s.undo();  // delete is undoable
  CHECK(s.station().size() == 1);

  CHECK_FALSE(s.deleteSelected());  // nothing selected now -> no-op
}

TEST_CASE("free-place fallback: non-empty station, ray misses modules -> standoff ghost") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);  // active = a_mod
  // Root-place a_mod at origin.
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  REQUIRE(s.commitGhost().has_value());

  s.cycleActive(1);  // b_mod
  // Down-ray far from the placed module -> no snap target -> free ghost at the standoff.
  s.updateGhost(Vec3{50, 10, 50}, Vec3{0, -1, 0});
  REQUIRE(s.ghost().has_value());
  CHECK(s.ghost()->valid);
  CHECK_FALSE(s.ghost()->candidate.has_value());  // free, not snapped
  CHECK(s.ghost()->worldTransform.position.x == doctest::Approx(50));
  CHECK(s.ghost()->worldTransform.position.z == doctest::Approx(50));
}

TEST_CASE("forceFree overrides an available snap target") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  REQUIRE(s.commitGhost().has_value());
  s.cycleActive(1);  // b_mod

  // Aim straight down at the placed a_mod. Normally snaps; forceFree -> free ghost.
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0}, /*forceFree=*/true);
  REQUIRE(s.ghost().has_value());
  CHECK_FALSE(s.ghost()->candidate.has_value());
  CHECK(s.ghost()->worldTransform.position.y == doctest::Approx(0));
}

TEST_CASE("rotateGhost orients the free placement ghost in 90-degree steps") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.rotateGhost(Vec3{0, 1, 0});  // +90 about Y
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});  // empty station -> free root ghost
  REQUIRE(s.ghost().has_value());
  const Vec3 r = rotate(s.ghost()->worldTransform.rotation, Vec3{1, 0, 0});
  CHECK(r.z == doctest::Approx(-1).epsilon(1e-9));  // +X rotated to -Z
}

TEST_CASE("loadStation replaces the document and resets history/selection/ghost") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);

  // Create undo history: root-place a_mod on the empty station.
  s.updateGhost(Vec3{0, 100, 0}, Vec3{0, -1, 0});  // ray straight down hits ground
  REQUIRE(s.ghost().has_value());
  REQUIRE(s.commitGhost().has_value());
  REQUIRE(s.canUndo());

  Station loaded;
  PlacedModule m;
  m.instanceId = 5;
  m.defId = "b_mod";
  loaded.add(m);

  s.loadStation(std::move(loaded));

  CHECK(s.station().size() == 1);
  CHECK(s.station().find(5) != nullptr);
  CHECK_FALSE(s.canUndo());
  CHECK_FALSE(s.canRedo());
  CHECK_FALSE(s.selected().has_value());
  CHECK_FALSE(s.ghost().has_value());
}

TEST_CASE("gizmo drag: free move of a lone module commits a MoveModuleCommand") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);  // a_mod
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  const InstanceId id = s.commitGhost().value();
  s.selectByRay(Vec3{0, 0, -10}, Vec3{0, 0, 1});
  REQUIRE(s.selected().value() == id);

  const double scale = 5.0;  // axisLength 5 -> handle spans x in [0,5]
  // Grab +X: ray down at x=2 hits (2,0,0) on the axis.
  REQUIRE(s.beginGizmoDrag(Vec3{2, 5, 0}, Vec3{0, -1, 0}, scale));
  REQUIRE(s.dragging());
  // Drag to x=4 -> delta +2 along X (lone module: no snap target).
  s.updateGizmoDrag(Vec3{4, 5, 0}, Vec3{0, -1, 0});
  REQUIRE(s.dragPreview().has_value());
  CHECK(s.dragPreview()->position.x == doctest::Approx(2));  // start 0 + 2

  CHECK(s.endGizmoDrag());
  CHECK_FALSE(s.dragging());
  CHECK(s.station().find(id)->worldTransform.position.x == doctest::Approx(2));
  CHECK(s.canUndo());
}

TEST_CASE("gizmo drag: snap-on-move re-links onto a neighbor") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);  // a_mod
  // a_mod root at origin.
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  const InstanceId ida = s.commitGhost().value();
  // b_mod free-placed away (forceFree avoids auto-snap).
  s.cycleActive(1);
  s.updateGhost(Vec3{100, 10, 0}, Vec3{0, -1, 0}, /*forceFree=*/true);
  const InstanceId idb = s.commitGhost().value();

  s.selectByRay(Vec3{100, 0, -10}, Vec3{0, 0, 1});
  REQUIRE(s.selected().value() == idb);

  const double scale = 5.0;
  REQUIRE(s.beginGizmoDrag(Vec3{102, 5, 0}, Vec3{0, -1, 0}, scale));  // +X of b's gizmo
  // Drag b toward the origin; within dragSnapRadius of a's connector -> snaps.
  s.updateGizmoDrag(Vec3{101, 5, 0}, Vec3{0, -1, 0});
  REQUIRE(s.endGizmoDrag());

  const PlacedModule* pb = s.station().find(idb);
  REQUIRE(pb != nullptr);
  REQUIRE(pb->links.size() == 1);
  CHECK(pb->links[0].otherInstanceId == ida);
  // Reciprocal on a.
  CHECK(s.station().find(ida)->links.size() == 1);
}

TEST_CASE("beginGizmoDrag is a no-op without a selection or a handle hit") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  const InstanceId id = s.commitGhost().value();
  // No selection yet.
  CHECK_FALSE(s.beginGizmoDrag(Vec3{2, 5, 0}, Vec3{0, -1, 0}, 5.0));
  // Select, but aim the ray far from any handle.
  s.selectByRay(Vec3{0, 0, -10}, Vec3{0, 0, 1});
  REQUIRE(s.selected().value() == id);
  CHECK_FALSE(s.beginGizmoDrag(Vec3{500, 500, 0}, Vec3{0, -1, 0}, 5.0));
}

TEST_CASE("rotateSelected re-orients a placed module via an undoable command") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  const InstanceId id = s.commitGhost().value();
  s.selectByRay(Vec3{0, 0, -10}, Vec3{0, 0, 1});

  REQUIRE(s.rotateSelected(Vec3{0, 1, 0}));  // +90 about Y
  const Vec3 r = rotate(s.station().find(id)->worldTransform.rotation, Vec3{1, 0, 0});
  CHECK(r.z == doctest::Approx(-1).epsilon(1e-9));
  CHECK(s.canUndo());
  s.undo();
  const Vec3 r0 = rotate(s.station().find(id)->worldTransform.rotation, Vec3{1, 0, 0});
  CHECK(r0.x == doctest::Approx(1).epsilon(1e-9));  // back to identity
}

TEST_CASE("gizmo hover: highlightHandle tracks the handle under the cursor") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  const InstanceId id = s.commitGhost().value();  // module at origin
  s.selectByRay(Vec3{0, 0, -10}, Vec3{0, 0, 1});
  REQUIRE(s.selected().value() == id);

  const double scale = 5.0;  // gizmo at origin, axisLength 5
  s.updateGizmoHover(Vec3{2, 5, 0}, Vec3{0, -1, 0}, scale);  // down the +X axis
  REQUIRE(s.highlightHandle().has_value());
  CHECK(*s.highlightHandle() == GizmoHandle::AxisX);

  s.updateGizmoHover(Vec3{500, 500, 0}, Vec3{0, -1, 0}, scale);  // misses every handle
  CHECK_FALSE(s.highlightHandle().has_value());
}

TEST_CASE("gizmo drag: a rotation ring spins the module in place and commits") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  const InstanceId id = s.commitGhost().value();  // module at origin
  s.selectByRay(Vec3{0, 0, -10}, Vec3{0, 0, 1});
  REQUIRE(s.selected().value() == id);
  s.setGizmoMode(GizmoMode::Rotate);  // rings are only pickable in Rotate mode

  const double scale = 5.0;  // ringRadius 5
  // Grab the RotY ring at a point off the axes: (3.5355, 0, 3.5355), r = 5.
  REQUIRE(s.beginGizmoDrag(Vec3{3.5355, 5, 3.5355}, Vec3{0, -1, 0}, scale));
  // Sweep the hit around to (5,0,0): a +45 deg rotation about +Y.
  s.updateGizmoDrag(Vec3{5, 5, 0}, Vec3{0, -1, 0});
  REQUIRE(s.dragPreview().has_value());
  CHECK(s.dragPreview()->position.x == doctest::Approx(0));  // rotation in place
  CHECK(s.dragPreview()->position.z == doctest::Approx(0));

  REQUIRE(s.endGizmoDrag());  // committed despite zero position change
  CHECK(s.canUndo());
  // +45 deg about +Y sends +X to (cos45, 0, -sin45).
  const Vec3 r = rotate(s.station().find(id)->worldTransform.rotation, Vec3{1, 0, 0});
  CHECK(r.z == doctest::Approx(-0.70710678).epsilon(1e-6));

  s.undo();  // rotation is undoable
  const Vec3 r0 = rotate(s.station().find(id)->worldTransform.rotation, Vec3{1, 0, 0});
  CHECK(r0.x == doctest::Approx(1).epsilon(1e-9));
}

TEST_CASE("EditorState rebuilds the connector grid after a placement") {
  ModuleCatalog catalog;
  ModuleDef a;
  a.id = "A";
  ConnectionPoint cp;
  cp.id = "a1";
  cp.localPosition = {0, 0, 0};
  a.connectionPoints.push_back(cp);
  catalog.add(a);

  EditorState state(catalog);

  // Free-place one module (forceFree skips the ray-pick snap path).
  state.updateGhost(Vec3{0, 0, 0}, Vec3{0, 0, 1}, /*forceFree=*/true);
  REQUIRE(state.ghost().has_value());
  REQUIRE(state.commitGhost().has_value());
  const std::size_t afterFirst = state.connectorGrid().size();
  CHECK(afterFirst == 1);

  // A second placement must dirty + rebuild the grid (not return the cached size).
  // Place far away so the collision check doesn't block it (this test is about
  // grid invalidation, not collision).
  state.updateGhost(Vec3{100, 0, 0}, Vec3{0, 0, 1}, /*forceFree=*/true);
  REQUIRE(state.commitGhost().has_value());
  CHECK(state.connectorGrid().size() == afterFirst + 1);
}

TEST_CASE("plot boundary box validation constraints") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);

  // 1. Ghost inside is valid, ghost outside (e.g. at 12000m) is invalid
  s.setPlaceDistance(100.0);
  s.updateGhost(Vec3{0, 0, 0}, Vec3{1, 0, 0}); // pos = 100,0,0
  REQUIRE(s.ghost().has_value());
  CHECK(s.ghost()->valid);

  s.setPlaceDistance(12000.0);
  s.updateGhost(Vec3{0, 0, 0}, Vec3{1, 0, 0}); // pos = 12000,0,0
  REQUIRE(s.ghost().has_value());
  CHECK(s.ghost()->valid); // Ghost is clamped inside, so it is valid!
  CHECK(s.ghost()->worldTransform.position.x == doctest::Approx(9998.5)); // 9999.0 - 0.5 (half-width)

  // Reset to default place distance
  s.setPlaceDistance(10.0);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0}); // pos = 0,0,0
  const InstanceId id = s.commitGhost().value();

  // 2. Dragging outside plot boundary should clamp the position
  s.selectByRay(Vec3{0, 0, -10}, Vec3{0, 0, 1});
  const double scale = 5.0;
  REQUIRE(s.beginGizmoDrag(Vec3{2, 5, 0}, Vec3{0, -1, 0}, scale)); // Grab X axis
  s.updateGizmoDrag(Vec3{12000, 5, 0}, Vec3{0, -1, 0}); // Drag to 12000 (outside)
  CHECK(s.endGizmoDrag()); // commit succeeds
  
  // Position is clamped to 9998.5
  CHECK(s.station().find(id)->worldTransform.position.x == doctest::Approx(9998.5));
}

TEST_CASE("gizmo mode: defaults to Translate, settable, resets when a module is selected") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);  // active = a_mod
  CHECK(s.gizmoMode() == GizmoMode::Translate);

  s.setGizmoMode(GizmoMode::Rotate);
  CHECK(s.gizmoMode() == GizmoMode::Rotate);

  // Place a_mod at the origin, switch to Rotate, then select it: selecting resets.
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  REQUIRE(s.commitGhost().has_value());
  s.setGizmoMode(GizmoMode::Rotate);
  s.selectByRay(Vec3{0, 10, 0}, Vec3{0, -1, 0});  // ray down hits the placed module
  REQUIRE(s.selected().has_value());
  CHECK(s.gizmoMode() == GizmoMode::Translate);
}

TEST_CASE("activeSnapLinks: shows the approaching connector pair before snapping") {
  const ModuleCatalog c = twoModuleCatalog();  // a_mod conn a1 at local (+0.5,0,0)
  EditorState s(c);  // active = a_mod

  // Root-place a_mod at the origin: its free connector a1 sits at world (0.5,0,0).
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  REQUIRE(s.commitGhost().has_value());

  // Active = b_mod (conn b1 at local (-0.5,0,0)). Free-place its ghost so b1 lands
  // ~50 units from a1 — inside the approach radius, but NOT a snap (forceFree).
  s.cycleActive(1);
  REQUIRE(s.activeDef()->id == "b_mod");
  s.setPlaceDistance(50.0);
  s.updateGhost(/*origin=*/Vec3{1, 0, 0}, /*dir=*/Vec3{0, 0, 1}, /*forceFree=*/true);
  REQUIRE(s.ghost().has_value());
  CHECK_FALSE(s.ghost()->candidate.has_value());  // free-placed, not snapped

  const std::vector<SnapLink> links = s.activeSnapLinks();
  REQUIRE(links.size() == 1);
  CHECK(links[0].fromWorld.x == doctest::Approx(0.5));   // b1 at the ghost pose
  CHECK(links[0].fromWorld.z == doctest::Approx(50.0));
  CHECK(links[0].toWorld.x == doctest::Approx(0.5));     // a1 on the placed module
  CHECK(links[0].toWorld.z == doctest::Approx(0.0));
}

TEST_CASE("activeSnapLinks: a guide-line for EVERY reachable target, not just the nearest") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);  // active = a_mod

  // Two free-placed a_mod instances: a1 connectors land at (0.5,0,0) and (0.5,0,100).
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  REQUIRE(s.commitGhost().has_value());
  s.setPlaceDistance(100.0);
  s.updateGhost(Vec3{0, 0, 0}, Vec3{0, 0, 1}, /*forceFree=*/true);  // origin -> (0,0,100)
  REQUIRE(s.commitGhost().has_value());
  REQUIRE(s.station().size() == 2);

  // b_mod ghost with b1 at (0.5,0,50): 50 units from BOTH a1 connectors (both in range).
  s.cycleActive(1);
  REQUIRE(s.activeDef()->id == "b_mod");
  s.setPlaceDistance(50.0);
  s.updateGhost(Vec3{1, 0, 0}, Vec3{0, 0, 1}, /*forceFree=*/true);  // b1 -> (0.5,0,50)
  REQUIRE(s.ghost().has_value());
  CHECK_FALSE(s.ghost()->candidate.has_value());

  const std::vector<SnapLink> links = s.activeSnapLinks();
  REQUIRE(links.size() == 2);  // BOTH targets get a guide-line, not just the nearest
  CHECK(links[0].fromWorld.x == doctest::Approx(0.5));  // both start at b1's world pos
  CHECK(links[0].fromWorld.z == doctest::Approx(50.0));
  CHECK(links[1].fromWorld.z == doctest::Approx(50.0));
  // The two targets are the two a1 connectors (z = 0 and 100), in either order.
  const double z0 = links[0].toWorld.z;
  const double z1 = links[1].toWorld.z;
  CHECK(((z0 == doctest::Approx(0.0) && z1 == doctest::Approx(100.0)) ||
         (z0 == doctest::Approx(100.0) && z1 == doctest::Approx(0.0))));
}

TEST_CASE("activeSnapLinks: empty when far, empty with no ghost/drag") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  CHECK(s.activeSnapLinks().empty());  // no ghost, no drag

  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  REQUIRE(s.commitGhost().has_value());
  s.cycleActive(1);  // b_mod
  s.setPlaceDistance(5000.0);
  s.updateGhost(Vec3{1, 0, 0}, Vec3{0, 0, 1}, /*forceFree=*/true);  // b1 ~5000 away
  CHECK(s.activeSnapLinks().empty());  // beyond lineRadius_
}

TEST_CASE("dock clearance always blocks placement, even with overlap allowed") {
  ModuleCatalog c;
  ModuleDef dock;
  dock.id = "dock";
  dock.category = Category::Dock;
  dock.aabb = AABB{{-1, -1, -1}, {1, 1, 1}};
  ClearanceVolume cv;
  cv.rotation = Quat{};
  cv.halfExtents = {5, 5, 50};
  cv.center = {0, 0, 50};  // corridor z[0,100], clear of the dock body at z[-1,1]
  cv.shipSize = "dock_m";
  dock.clearanceVolumes.push_back(cv);
  c.add(dock);
  ModuleDef box;
  box.id = "box";
  box.category = Category::Storage;
  box.aabb = AABB{{-2, -2, -2}, {2, 2, 2}};
  c.add(box);

  EditorState s(c);
  // Place the dock at origin (free-place "dock" first).
  while (s.activeDef()->id != "dock") s.cycleActive(1);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0}, /*forceFree=*/true);
  REQUIRE(s.commitGhost().has_value());

  // Free-place "box" inside the dock's corridor at z~50 (clear of the dock body,
  // so only clearance — not body overlap — can block it).
  while (s.activeDef()->id != "box") s.cycleActive(1);
  s.setPlaceDistance(50.0);
  s.updateGhost(Vec3{0, 0, -1}, Vec3{0, 0, 1}, /*forceFree=*/true);  // standoff lands near z=49
  REQUIRE(s.ghost().has_value());
  CHECK_FALSE(s.ghost()->valid);  // blocked by clearance

  // The overlap toggle relaxes body overlap only — clearance still blocks.
  s.setAllowOverlap(true);
  s.updateGhost(Vec3{0, 0, -1}, Vec3{0, 0, 1}, /*forceFree=*/true);
  REQUIRE(s.ghost().has_value());
  CHECK_FALSE(s.ghost()->valid);  // STILL blocked (clearance is not bypassed)
}

TEST_CASE("overlap is blocked by default and allowed by the toggle") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);  // active = a_mod (AABB +/-0.5)

  // Place a_mod at the origin.
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});
  REQUIRE(s.commitGhost().has_value());

  // Free-place another a_mod overlapping the first (ray down onto origin, Alt = forceFree).
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0}, /*forceFree=*/true);
  REQUIRE(s.ghost().has_value());
  CHECK_FALSE(s.ghost()->valid);            // overlaps -> invalid
  CHECK_FALSE(s.commitGhost().has_value()); // commit rejected

  s.setAllowOverlap(true);
  CHECK(s.allowOverlap());
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0}, /*forceFree=*/true);
  REQUIRE(s.ghost().has_value());
  CHECK(s.ghost()->valid);                  // bypassed -> valid
  CHECK(s.commitGhost().has_value());
}

TEST_CASE("showAllClearance is render-only state: defaults off, round-trips") {
  const ModuleCatalog c;
  EditorState s(c);
  CHECK_FALSE(s.showAllClearance());
  s.setShowAllClearance(true);
  CHECK(s.showAllClearance());
  s.setShowAllClearance(false);
  CHECK_FALSE(s.showAllClearance());
}


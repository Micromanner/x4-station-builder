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

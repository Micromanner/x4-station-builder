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

TEST_CASE("root ghost is absent when the ray misses the ground plane") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, 1, 0});  // pointing up, away from ground
  CHECK_FALSE(s.ghost().has_value());
  CHECK(s.commitGhost() == std::nullopt);
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

TEST_CASE("snap ghost absent when the ray hits no module") {
  const ModuleCatalog c = twoModuleCatalog();
  EditorState s(c);
  s.updateGhost(Vec3{0, 10, 0}, Vec3{0, -1, 0});  // root
  REQUIRE(s.commitGhost().has_value());
  s.cycleActive(1);
  s.updateGhost(Vec3{100, 100, 100}, Vec3{0, 1, 0});  // points away from everything
  CHECK_FALSE(s.ghost().has_value());
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

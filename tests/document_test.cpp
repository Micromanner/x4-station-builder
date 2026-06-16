#include "x4sb/document/station.hpp"
#include "x4sb/document/commands.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <memory>

using namespace x4sb;

namespace {
// Minimal command used to exercise the undo stack.
struct AddModule : Command {
  PlacedModule module;
  InstanceId placedId{0};
  explicit AddModule(PlacedModule m) : module(std::move(m)) {}
  void apply(Station& s) override { placedId = s.add(module); }
  void undo(Station& s) override { s.remove(placedId); }
};
}  // namespace

TEST_CASE("Station assigns instance ids and finds modules") {
  Station s;
  PlacedModule m;
  m.defId = "prod_module";
  const InstanceId id = s.add(m);
  CHECK(id != 0);
  CHECK(s.size() == 1);
  REQUIRE(s.find(id) != nullptr);
  CHECK(s.find(id)->defId == "prod_module");
}

TEST_CASE("undo and redo of an add command") {
  Station s;
  UndoStack stack;
  PlacedModule m;
  m.defId = "x";

  stack.execute(s, std::make_unique<AddModule>(m));
  CHECK(s.size() == 1);
  CHECK(stack.canUndo());

  stack.undo(s);
  CHECK(s.empty());
  CHECK(stack.canRedo());

  stack.redo(s);
  CHECK(s.size() == 1);
}

TEST_CASE("PlaceModuleCommand adds a snapped module with reciprocal links") {
  Station s;
  UndoStack stack;
  PlacedModule a;
  a.defId = "A";
  const InstanceId idA = s.add(a);

  // Place B snapped to A: B's "b1" onto A's "a1".
  stack.execute(s, std::make_unique<PlaceModuleCommand>("B", Transform{}, idA, "b1", "a1"));
  REQUIRE(s.size() == 2);

  InstanceId idB = 0;
  for (const auto& m : s.modules())
    if (m.instanceId != idA) idB = m.instanceId;
  REQUIRE(idB != 0);

  const PlacedModule* b = s.find(idB);
  REQUIRE(b->links.size() == 1);
  CHECK(b->links[0].thisPointId == "b1");
  CHECK(b->links[0].otherInstanceId == idA);
  CHECK(b->links[0].otherPointId == "a1");

  const PlacedModule* a2 = s.find(idA);
  REQUIRE(a2->links.size() == 1);
  CHECK(a2->links[0].thisPointId == "a1");
  CHECK(a2->links[0].otherInstanceId == idB);
  CHECK(a2->links[0].otherPointId == "b1");

  stack.undo(s);
  CHECK(s.size() == 1);
  CHECK(s.find(idA)->links.empty());

  stack.redo(s);
  CHECK(s.size() == 2);
  CHECK(s.find(idA)->links.size() == 1);
}

TEST_CASE("PlaceModuleCommand places a free (unlinked) module") {
  Station s;
  UndoStack stack;
  stack.execute(s, std::make_unique<PlaceModuleCommand>("Solo", Transform{}));
  REQUIRE(s.size() == 1);
  const InstanceId id = s.modules().front().instanceId;
  CHECK(s.find(id)->defId == "Solo");
  CHECK(s.find(id)->links.empty());

  stack.undo(s);
  CHECK(s.empty());
  stack.redo(s);
  CHECK(s.size() == 1);
  CHECK(s.find(id)->links.empty());
}

TEST_CASE("DeleteModuleCommand removes a module and restores neighbor links on undo") {
  Station s;
  UndoStack stack;
  PlacedModule a;
  a.defId = "A";
  const InstanceId idA = s.add(a);
  stack.execute(s, std::make_unique<PlaceModuleCommand>("B", Transform{}, idA, "b1", "a1"));

  InstanceId idB = 0;
  for (const auto& m : s.modules())
    if (m.instanceId != idA) idB = m.instanceId;
  REQUIRE(idB != 0);

  stack.execute(s, std::make_unique<DeleteModuleCommand>(idB));
  CHECK(s.size() == 1);
  CHECK(s.find(idB) == nullptr);
  CHECK(s.find(idA)->links.empty());  // reciprocal stripped

  stack.undo(s);
  REQUIRE(s.size() == 2);
  REQUIRE(s.find(idB) != nullptr);
  CHECK(s.find(idB)->links.size() == 1);  // B's own link restored
  CHECK(s.find(idA)->links.size() == 1);  // A's reciprocal restored
}

TEST_CASE("DeleteModuleCommand strips and restores links to multiple neighbors") {
  Station s;
  // Build A <-> B <-> C by hand (B has two reciprocal link pairs).
  PlacedModule a;
  a.defId = "A";
  const InstanceId idA = s.add(a);
  PlacedModule c;
  c.defId = "C";
  const InstanceId idC = s.add(c);
  PlacedModule b;
  b.defId = "B";
  b.links.push_back(Link{"b_to_a", idA, "a1"});
  b.links.push_back(Link{"b_to_c", idC, "c1"});
  const InstanceId idB = s.add(b);
  s.find(idA)->links.push_back(Link{"a1", idB, "b_to_a"});
  s.find(idC)->links.push_back(Link{"c1", idB, "b_to_c"});

  UndoStack stack;
  stack.execute(s, std::make_unique<DeleteModuleCommand>(idB));
  CHECK(s.find(idB) == nullptr);
  CHECK(s.find(idA)->links.empty());  // both reciprocals stripped
  CHECK(s.find(idC)->links.empty());

  stack.undo(s);
  REQUIRE(s.find(idB) != nullptr);
  CHECK(s.find(idB)->links.size() == 2);  // B's two own links restored
  CHECK(s.find(idA)->links.size() == 1);  // A reciprocal restored
  CHECK(s.find(idC)->links.size() == 1);  // C reciprocal restored

  // Redo cycle: re-delete, then undo again, both consistent.
  stack.redo(s);
  CHECK(s.find(idB) == nullptr);
  CHECK(s.find(idA)->links.empty());
  CHECK(s.find(idC)->links.empty());
  stack.undo(s);
  REQUIRE(s.find(idB) != nullptr);
  CHECK(s.find(idB)->links.size() == 2);
  CHECK(s.find(idA)->links.size() == 1);
  CHECK(s.find(idC)->links.size() == 1);
}

TEST_CASE("MoveModuleCommand repositions and detaches links") {
  Station s;
  UndoStack stack;
  PlacedModule a;
  a.defId = "A";
  const InstanceId idA = s.add(a);
  stack.execute(s, std::make_unique<PlaceModuleCommand>("B", Transform{}, idA, "b1", "a1"));

  InstanceId idB = 0;
  for (const auto& m : s.modules())
    if (m.instanceId != idA) idB = m.instanceId;
  REQUIRE(idB != 0);

  Transform moved;
  moved.position = {100, 0, 0};
  stack.execute(s, std::make_unique<MoveModuleCommand>(idB, moved));
  CHECK(s.find(idB)->worldTransform.position.x == doctest::Approx(100));
  CHECK(s.find(idB)->links.empty());  // detached
  CHECK(s.find(idA)->links.empty());  // reciprocal stripped

  stack.undo(s);
  CHECK(s.find(idB)->worldTransform.position.x == doctest::Approx(0));
  CHECK(s.find(idB)->links.size() == 1);  // restored
  CHECK(s.find(idA)->links.size() == 1);
}

TEST_CASE("MoveModuleCommand detaches multiple neighbors and restores transform fully") {
  Station s;
  PlacedModule a;
  a.defId = "A";
  const InstanceId idA = s.add(a);
  PlacedModule c;
  c.defId = "C";
  const InstanceId idC = s.add(c);
  PlacedModule b;
  b.defId = "B";
  const double rc = std::sqrt(2.0) / 2.0;
  b.worldTransform.position = {1, 2, 3};
  b.worldTransform.rotation = Quat{rc, 0, 0, rc};  // 90deg about Z
  b.links.push_back(Link{"b_to_a", idA, "a1"});
  b.links.push_back(Link{"b_to_c", idC, "c1"});
  const InstanceId idB = s.add(b);
  s.find(idA)->links.push_back(Link{"a1", idB, "b_to_a"});
  s.find(idC)->links.push_back(Link{"c1", idB, "b_to_c"});

  UndoStack stack;
  Transform moved;
  moved.position = {100, 0, 0};
  stack.execute(s, std::make_unique<MoveModuleCommand>(idB, moved));
  CHECK(s.find(idB)->links.empty());
  CHECK(s.find(idA)->links.empty());
  CHECK(s.find(idC)->links.empty());
  CHECK(s.find(idB)->worldTransform.position.x == doctest::Approx(100));

  stack.undo(s);
  CHECK(s.find(idB)->links.size() == 2);
  CHECK(s.find(idA)->links.size() == 1);
  CHECK(s.find(idC)->links.size() == 1);
  CHECK(s.find(idB)->worldTransform.position.x == doctest::Approx(1));
  CHECK(s.find(idB)->worldTransform.position.y == doctest::Approx(2));
  CHECK(s.find(idB)->worldTransform.rotation.w == doctest::Approx(rc));
  CHECK(s.find(idB)->worldTransform.rotation.z == doctest::Approx(rc));

  stack.redo(s);
  CHECK(s.find(idB)->links.empty());
  CHECK(s.find(idA)->links.empty());
  CHECK(s.find(idC)->links.empty());
  stack.undo(s);
  CHECK(s.find(idB)->links.size() == 2);
  CHECK(s.find(idA)->links.size() == 1);
  CHECK(s.find(idC)->links.size() == 1);
}

TEST_CASE("MoveModuleCommand does not duplicate a self-link on undo") {
  Station s;
  PlacedModule m;
  m.defId = "M";
  const InstanceId id = s.add(m);
  s.find(id)->links.push_back(Link{"p", id, "q"});  // hand-built degenerate self-link

  UndoStack stack;
  Transform moved;
  moved.position = {5, 0, 0};
  stack.execute(s, std::make_unique<MoveModuleCommand>(id, moved));
  CHECK(s.find(id)->links.empty());

  stack.undo(s);
  CHECK(s.find(id)->links.size() == 1);  // restored exactly once (guard prevents duplication)
}

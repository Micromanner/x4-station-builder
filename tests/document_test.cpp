#include "x4sb/document/station.hpp"

#include <doctest/doctest.h>

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

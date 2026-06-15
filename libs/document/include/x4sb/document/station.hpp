#pragma once
// The document model (spec §4B.2 / §5): the Station being edited — the single
// source of truth — plus an undo/redo command stack.
#include "x4sb/data/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x4sb {

using InstanceId = std::uint64_t;

struct Link {
  std::string thisPointId;
  InstanceId otherInstanceId{0};
  std::string otherPointId;
};

struct PlacedModule {
  InstanceId instanceId{0};
  std::string defId;  // -> ModuleDef::id
  Transform worldTransform{};
  std::vector<Link> links;
};

class Station {
 public:
  // Add a module. If instanceId is 0 a fresh id is assigned. Returns the id used.
  InstanceId add(const PlacedModule& m);
  bool remove(InstanceId id);
  PlacedModule* find(InstanceId id);
  [[nodiscard]] const PlacedModule* find(InstanceId id) const;

  [[nodiscard]] const std::vector<PlacedModule>& modules() const { return placed_; }
  [[nodiscard]] std::size_t size() const { return placed_.size(); }
  [[nodiscard]] bool empty() const { return placed_.empty(); }

 private:
  std::vector<PlacedModule> placed_;
  InstanceId nextId_{1};
};

// One reversible edit. Concrete commands (place/move/delete/link) implement this.
class Command {
 public:
  virtual ~Command() = default;
  virtual void apply(Station& s) = 0;
  virtual void undo(Station& s) = 0;
};

class UndoStack {
 public:
  // Apply a command and record it for undo. Clears the redo history.
  void execute(Station& s, std::unique_ptr<Command> cmd);
  void undo(Station& s);
  void redo(Station& s);
  [[nodiscard]] bool canUndo() const { return !done_.empty(); }
  [[nodiscard]] bool canRedo() const { return !undone_.empty(); }

 private:
  std::vector<std::unique_ptr<Command>> done_;
  std::vector<std::unique_ptr<Command>> undone_;
};

}  // namespace x4sb

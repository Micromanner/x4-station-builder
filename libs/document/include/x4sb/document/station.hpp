#pragma once
// The document model (spec §4B.2 / §5): the Station being edited — the single
// source of truth — plus an undo/redo command stack.
#include "x4sb/data/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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
  // Changes on every mutation and is drawn from one process-wide counter, so two
  // stations (or two states of one) never share a value unless their content is
  // equal (a copy keeps its source's value until either side mutates). Render
  // caches key on it instead of hashing every module per frame.
  [[nodiscard]] std::uint64_t revision() const { return revision_; }

 private:
  // id -> position in placed_, so find() is O(1) instead of a linear scan. The
  // snap search resolves a connector's owning module per grid candidate, thousands
  // of times per frame on a dense station, so a linear find() there silently turned
  // the grid-accelerated path back into O(modules) per candidate (known-issues 1.2
  // follow-up). Maintained only by add()/remove(); find() returns a mutable element
  // for in-place edits, which never change ids, so the map stays valid.
  void reindex();
  void touch();
  std::vector<PlacedModule> placed_;
  std::unordered_map<InstanceId, std::size_t> index_;
  InstanceId nextId_{1};
  std::uint64_t revision_{nextRevision()};
  [[nodiscard]] static std::uint64_t nextRevision();
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

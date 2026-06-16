#pragma once
// Concrete reversible edits on a Station (spec §5). Each command stores exactly
// what undo needs and preserves Link reciprocity (every Link has a matching entry
// on the other module). Pure data/logic; no rendering.
#include "x4sb/document/station.hpp"

#include <string>
#include <utility>
#include <vector>

namespace x4sb {

// Add a module, optionally snapped to an existing connector (records the
// reciprocal Link pair). The unlinked overload places a free module.
class PlaceModuleCommand : public Command {
 public:
  PlaceModuleCommand(std::string defId, Transform worldTransform);
  PlaceModuleCommand(std::string defId, Transform worldTransform, InstanceId targetInstanceId,
                     std::string newPointId, std::string targetPointId);
  void apply(Station& s) override;
  void undo(Station& s) override;

 private:
  std::string defId_;
  Transform worldTransform_;
  bool snapped_{false};
  InstanceId targetInstanceId_{0};
  std::string newPointId_;
  std::string targetPointId_;
  InstanceId placedId_{0};  // captured on first apply; reused on redo
};

// Remove a module and every Link referencing it (its own + neighbors'). Undo
// restores the module and all stripped links.
class DeleteModuleCommand : public Command {
 public:
  explicit DeleteModuleCommand(InstanceId id);
  void apply(Station& s) override;
  void undo(Station& s) override;

 private:
  InstanceId id_;
  PlacedModule removed_;  // captured on apply for restore
  bool captured_{false};  // false if apply() found no such module (undo then no-ops)
  std::vector<std::pair<InstanceId, Link>> strippedNeighborLinks_;
};

// Set a module's world transform (free reposition). Moving detaches: it removes
// the module's links and their reciprocals. Undo restores transform and links.
class MoveModuleCommand : public Command {
 public:
  MoveModuleCommand(InstanceId id, Transform newTransform);
  void apply(Station& s) override;
  void undo(Station& s) override;

 private:
  InstanceId id_;
  Transform newTransform_;
  Transform oldTransform_;
  bool captured_{false};
  std::vector<Link> removedOwnLinks_;
  std::vector<std::pair<InstanceId, Link>> strippedNeighborLinks_;
};

}  // namespace x4sb

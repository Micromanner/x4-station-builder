#include "x4sb/document/commands.hpp"

#include <utility>
#include <vector>

namespace x4sb {
namespace {

// Strip every link pointing back at `id` from each neighbor referenced by
// `ownLinks`, returning the removed (neighborId, Link) pairs so undo can restore
// them. Self-links are skipped — the owning module's own links are dropped by the
// caller, so recording them here would double-count. Preserves Link reciprocity.
std::vector<std::pair<InstanceId, Link>> stripNeighborLinks(Station& s, InstanceId id,
                                                            const std::vector<Link>& ownLinks) {
  std::vector<std::pair<InstanceId, Link>> stripped;
  for (const Link& l : ownLinks) {
    if (l.otherInstanceId == id) continue;
    PlacedModule* nb = s.find(l.otherInstanceId);
    if (!nb) continue;
    auto& links = nb->links;
    for (auto it = links.begin(); it != links.end();) {
      if (it->otherInstanceId == id) {
        stripped.emplace_back(nb->instanceId, *it);
        it = links.erase(it);
      } else {
        ++it;
      }
    }
  }
  return stripped;
}

}  // namespace

PlaceModuleCommand::PlaceModuleCommand(std::string defId, Transform worldTransform)
    : defId_(std::move(defId)), worldTransform_(worldTransform) {}

PlaceModuleCommand::PlaceModuleCommand(std::string defId, Transform worldTransform,
                                       InstanceId targetInstanceId, std::string newPointId,
                                       std::string targetPointId)
    : defId_(std::move(defId)),
      worldTransform_(worldTransform),
      snapped_(true),
      targetInstanceId_(targetInstanceId),
      newPointId_(std::move(newPointId)),
      targetPointId_(std::move(targetPointId)) {}

void PlaceModuleCommand::apply(Station& s) {
  PlacedModule m;
  m.instanceId = placedId_;  // 0 -> Station assigns; reused id keeps redo stable
  m.defId = defId_;
  m.worldTransform = worldTransform_;
  // Only record the snap link if the target actually exists, so we never leave a
  // one-way (dangling) link — preserves the reciprocity invariant.
  const bool linkToTarget = snapped_ && s.find(targetInstanceId_) != nullptr;
  if (linkToTarget) m.links.push_back(Link{newPointId_, targetInstanceId_, targetPointId_});
  placedId_ = s.add(m);
  if (linkToTarget) {
    if (PlacedModule* target = s.find(targetInstanceId_))
      target->links.push_back(Link{targetPointId_, placedId_, newPointId_});
  }
}

void PlaceModuleCommand::undo(Station& s) {
  if (snapped_) {
    if (PlacedModule* target = s.find(targetInstanceId_)) {
      auto& links = target->links;
      for (auto it = links.begin(); it != links.end();) {
        if (it->otherInstanceId == placedId_ && it->thisPointId == targetPointId_)
          it = links.erase(it);
        else
          ++it;
      }
    }
  }
  s.remove(placedId_);
}

DeleteModuleCommand::DeleteModuleCommand(InstanceId id) : id_(id) {}

void DeleteModuleCommand::apply(Station& s) {
  const PlacedModule* m = s.find(id_);
  if (!m) return;
  removed_ = *m;  // includes its own outgoing links
  captured_ = true;

  strippedNeighborLinks_ = stripNeighborLinks(s, id_, removed_.links);
  s.remove(id_);
}

void DeleteModuleCommand::undo(Station& s) {
  if (!captured_) return;
  s.add(removed_);  // restores the same instanceId and its outgoing links
  for (const auto& entry : strippedNeighborLinks_) {
    if (PlacedModule* nb = s.find(entry.first)) nb->links.push_back(entry.second);
  }
}

MoveModuleCommand::MoveModuleCommand(InstanceId id, Transform newTransform)
    : id_(id), newTransform_(newTransform) {}

void MoveModuleCommand::apply(Station& s) {
  PlacedModule* m = s.find(id_);
  if (!m) return;
  oldTransform_ = m->worldTransform;
  removedOwnLinks_ = m->links;
  captured_ = true;

  // stripNeighborLinks only mutates neighbors' link vectors, never Station::placed_,
  // so `m` stays valid across the call.
  strippedNeighborLinks_ = stripNeighborLinks(s, id_, removedOwnLinks_);
  m->links.clear();
  m->worldTransform = newTransform_;
}

void MoveModuleCommand::undo(Station& s) {
  if (!captured_) return;
  PlacedModule* m = s.find(id_);
  if (!m) return;
  m->worldTransform = oldTransform_;
  m->links = removedOwnLinks_;
  for (const auto& entry : strippedNeighborLinks_) {
    if (PlacedModule* nb = s.find(entry.first)) nb->links.push_back(entry.second);
  }
}

}  // namespace x4sb

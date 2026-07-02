#include "x4sb/autolayout/moves.hpp"

#include "x4sb/snap/snap.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace x4sb {
namespace {

// A movable leaf: id >= minMovableId, exactly one parent (a lower-id link), no child link.
struct Leaf {
  InstanceId id = 0;
  InstanceId parentId = 0;    // current parent (lower id)
  std::string thisPointId;    // the leaf's connector mated to the parent
  std::string parentPointId;  // the parent's connector
};

std::vector<Leaf> movableLeaves(const Station& s, InstanceId minMovableId) {
  std::vector<Leaf> leaves;
  for (const auto& m : s.modules()) {
    if (m.instanceId < minMovableId) continue;
    bool hasChild = false;
    Leaf leaf;
    leaf.id = m.instanceId;
    bool hasParent = false;
    for (const Link& l : m.links) {
      if (l.otherInstanceId > m.instanceId) {
        hasChild = true;
        break;
      }
      // a lower-id link is the parent (a snapped leaf has exactly one)
      hasParent = true;
      leaf.parentId = l.otherInstanceId;
      leaf.thisPointId = l.thisPointId;
      leaf.parentPointId = l.otherPointId;
    }
    if (hasParent && !hasChild) leaves.push_back(leaf);
  }
  std::sort(leaves.begin(), leaves.end(),
            [](const Leaf& a, const Leaf& b) { return a.id < b.id; });  // deterministic order
  return leaves;
}

struct Mate {
  InstanceId targetId = 0;
  std::string targetPointId;
  std::string newPointId;  // the leaf's connector to use
};

// Free compatible connectors on modules with id < leafId, excluding the leaf's CURRENT
// mate (so the move actually changes something). Deterministic order.
std::vector<Mate> altMates(const Station& s, const ModuleCatalog& c, const ModuleDef& leafDef,
                           const Leaf& leaf) {
  std::vector<Mate> mates;
  for (const auto& t : s.modules()) {
    if (t.instanceId >= leaf.id) continue;
    const ModuleDef* td = c.find(t.defId);
    if (td == nullptr) continue;
    for (const auto& tp : td->connectionPoints) {
      if (connectorIsLinked(t, tp.id)) continue;
      for (const auto& np : leafDef.connectionPoints) {
        if (!connectorsCompatible(tp, np)) continue;
        const bool isCurrent = t.instanceId == leaf.parentId && tp.id == leaf.parentPointId &&
                               np.id == leaf.thisPointId;
        if (isCurrent) continue;
        mates.push_back(Mate{t.instanceId, tp.id, np.id});
      }
    }
  }
  std::sort(mates.begin(), mates.end(), [](const Mate& a, const Mate& b) {
    if (a.targetId != b.targetId) return a.targetId < b.targetId;
    if (a.targetPointId != b.targetPointId) return a.targetPointId < b.targetPointId;
    return a.newPointId < b.newPointId;
  });
  return mates;
}

void detachFromParent(Station& s, const Leaf& leaf) {
  PlacedModule* m = s.find(leaf.id);
  PlacedModule* parent = s.find(leaf.parentId);
  if (m != nullptr) {
    auto& ml = m->links;
    ml.erase(std::remove_if(ml.begin(), ml.end(),
                            [&](const Link& l) { return l.otherInstanceId == leaf.parentId; }),
             ml.end());
  }
  if (parent != nullptr) {
    auto& pl = parent->links;
    pl.erase(std::remove_if(pl.begin(), pl.end(),
                            [&](const Link& l) { return l.otherInstanceId == leaf.id; }),
             pl.end());
  }
}

}  // namespace

std::optional<Station> rehostMove(const Station& station, const ModuleCatalog& catalog,
                                  std::mt19937& rng, InstanceId minMovableId) {
  const std::vector<Leaf> leaves = movableLeaves(station, minMovableId);
  if (leaves.empty()) return std::nullopt;
  const Leaf& leaf = leaves[std::uniform_int_distribution<std::size_t>(0, leaves.size() - 1)(rng)];

  const ModuleDef* leafDef = catalog.find(station.find(leaf.id)->defId);
  if (leafDef == nullptr) return std::nullopt;

  const std::vector<Mate> mates = altMates(station, catalog, *leafDef, leaf);
  if (mates.empty()) return std::nullopt;
  const Mate& mate = mates[std::uniform_int_distribution<std::size_t>(0, mates.size() - 1)(rng)];

  Station out = station;
  detachFromParent(out, leaf);
  const Transform xf = computeSnapTransform(out, catalog, mate.targetId, mate.targetPointId,
                                            *leafDef, mate.newPointId);
  PlacedModule* m = out.find(leaf.id);
  m->worldTransform = xf;
  m->links.push_back(Link{mate.newPointId, mate.targetId, mate.targetPointId});
  if (PlacedModule* t = out.find(mate.targetId))
    t->links.push_back(Link{mate.targetPointId, leaf.id, mate.newPointId});
  return out;
}

}  // namespace x4sb

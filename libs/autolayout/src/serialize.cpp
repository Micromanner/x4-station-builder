#include "x4sb/autolayout/serialize.hpp"

#include <algorithm>
#include <unordered_set>

namespace x4sb {

std::vector<LayoutPlacement> extractPlacements(const Station& work, const Station& existing,
                                               const ModuleCatalog& catalog) {
  (void)catalog;  // extraction reads the link graph; the catalog is part of the stable signature
  std::unordered_set<InstanceId> existingIds;
  for (const auto& m : existing.modules()) existingIds.insert(m.instanceId);

  std::vector<const PlacedModule*> added;
  for (const auto& m : work.modules())
    if (existingIds.find(m.instanceId) == existingIds.end()) added.push_back(&m);
  std::sort(added.begin(), added.end(), [](const PlacedModule* a, const PlacedModule* b) {
    return a->instanceId < b->instanceId;
  });

  std::vector<LayoutPlacement> out;
  out.reserve(added.size());
  for (const PlacedModule* m : added) {
    // The parent is the unique lower-id link (seed adds parents first; rehostMove keeps
    // re-parenting lower-id). No lower-id link => a floating root.
    const Link* parent = nullptr;
    for (const Link& l : m->links)
      if (l.otherInstanceId < m->instanceId) {
        parent = &l;
        break;
      }
    if (parent != nullptr)
      out.push_back(LayoutPlacement{m->defId, m->worldTransform, true, parent->otherInstanceId,
                                    parent->thisPointId, parent->otherPointId, m->instanceId});
    else
      out.push_back(LayoutPlacement{m->defId, m->worldTransform, false, 0, "", "", m->instanceId});
  }
  return out;
}

}  // namespace x4sb

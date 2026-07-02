#include "x4sb/autolayout/search.hpp"

#include "x4sb/autolayout/autolayout.hpp"  // AutoLayoutOptions (full definition)
#include "x4sb/data/math.hpp"
#include "x4sb/snap/snap.hpp"

#include <algorithm>

namespace x4sb {
namespace {

bool insidePlot(const AABB& box, double h) {
  return box.min.x >= -h && box.max.x <= h && box.min.y >= -h && box.max.y <= h &&
         box.min.z >= -h && box.max.z <= h;
}

}  // namespace

bool layoutLegal(const Station& station, const ModuleCatalog& catalog,
                 const AutoLayoutOptions& opts) {
  for (const auto& m : station.modules()) {
    const ModuleDef* dm = catalog.find(m.defId);
    if (dm == nullptr) continue;
    const AABB wm = worldAabb(dm->aabb, m.worldTransform);
    if (!insidePlot(wm, opts.plotHalfExtent)) return false;

    // Clearance: m's corridors vs every other body, and every other corridor vs m's
    // body, skipping only m itself. The snap partner is NOT exempt (a corridor that
    // re-enters its mount target is still an obstruction, spec §10).
    if (collidesClearance(*dm, m.worldTransform, m.instanceId, 0, station, catalog)) return false;

    for (const auto& o : station.modules()) {
      if (o.instanceId == m.instanceId) continue;
      // Scans only m.links; assumes every link is written on both sides (all creation
      // sites do this today) — a one-sided link would wrongly flag a real joint as an overlap.
      const bool linked = std::any_of(m.links.begin(), m.links.end(), [&](const Link& l) {
        return l.otherInstanceId == o.instanceId;
      });
      if (linked) continue;  // directly-joined bodies touch at the joint by construction
      const ModuleDef* doo = catalog.find(o.defId);
      if (doo == nullptr) continue;
      if (overlaps(wm, worldAabb(doo->aabb, o.worldTransform))) return false;
    }
  }
  return true;
}

}  // namespace x4sb

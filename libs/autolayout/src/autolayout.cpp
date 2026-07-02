#include "x4sb/autolayout/autolayout.hpp"

#include "x4sb/autolayout/peripheral.hpp"
#include "x4sb/autolayout/roles.hpp"
#include "x4sb/autolayout/seed.hpp"
#include "x4sb/autolayout/serialize.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace x4sb {
namespace {

int categoryPriority(Category c) {
  switch (c) {
    case Category::Connector:
      return 0;
    case Category::Production:
      return 1;
    case Category::Storage:
      return 2;
    case Category::Habitat:
      return 3;
    case Category::Defense:
      return 4;
    case Category::Other:
      return 5;
    case Category::Dock:
      return 6;
  }
  return 5;
}

double aabbVolume(const AABB& b) {
  const Vec3 d = b.max - b.min;
  return std::abs(d.x * d.y * d.z);
}

void sortUnique(std::vector<std::string>& v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

struct RoleSplit {
  std::vector<std::string> interior;
  std::vector<std::pair<int, std::string>> peripheralRanked;  // (rank docks<turrets<solar, id)
};

RoleSplit splitRoles(const OrderedCart& oc, const ModuleCatalog& catalog) {
  RoleSplit split;
  for (const std::string& id : oc.placement) {
    const ModuleDef* d = catalog.find(id);
    if (d == nullptr) continue;
    const Role role = classifyRole(*d);
    if (!isPeripheral(role)) {
      split.interior.push_back(id);
      continue;
    }
    const int rank = role == Role::Dock ? 0 : (role == Role::Turret ? 1 : 2);
    split.peripheralRanked.push_back({rank, id});
  }
  std::stable_sort(split.peripheralRanked.begin(), split.peripheralRanked.end(),
                   [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) {
                     return a.first < b.first;
                   });
  return split;
}

}  // namespace

OrderedCart orderedPlacement(const QuantityList& cart, const ModuleCatalog& catalog,
                             const AutoLayoutOptions& opts) {
  OrderedCart out;
  for (const auto& [id, count] : cart) {
    if (count <= 0) continue;
    out.requested += count;
    const ModuleDef* d = catalog.find(id);
    if (d == nullptr || !d->playerBuildable) {
      out.skipped.push_back(id);
      continue;
    }
    for (int i = 0; i < count; ++i) out.placement.push_back(id);
  }
  if (opts.shuffle) {
    std::mt19937 g(opts.seed);
    std::shuffle(out.placement.begin(), out.placement.end(), g);
  } else {
    std::sort(out.placement.begin(), out.placement.end(),
              [&](const std::string& a, const std::string& b) {
                const ModuleDef* da = catalog.find(a);  // non-null: only valid ids reach here
                const ModuleDef* db = catalog.find(b);
                const int pa = categoryPriority(da->category);
                const int pb = categoryPriority(db->category);
                if (pa != pb) return pa < pb;
                const double va = aabbVolume(da->aabb);
                const double vb = aabbVolume(db->aabb);
                if (va > vb) return true;  // larger first (no float == comparison)
                if (va < vb) return false;
                return a < b;
              });
  }
  sortUnique(out.skipped);
  return out;
}

// §5 pipeline: classify -> planned fishbone skeleton seed -> peripheral (docks first) -> serialize.
// Additive: the seed only adds the cart onto existing structure, never moves placed modules. The
// fishbone is tidy and legal by construction, so there is no post-seed anneal to un-tidy it.
AutoLayoutResult autoLayout(const Station& existing, const QuantityList& cart,
                            const ModuleCatalog& catalog, const AutoLayoutOptions& opts) {
  AutoLayoutResult result;
  const OrderedCart oc = orderedPlacement(cart, catalog, opts);
  result.report.requested = oc.requested;
  result.report.skippedDefs = oc.skipped;
  const RoleSplit split = splitRoles(oc, catalog);

  Station work = existing;
  result.report.connectorsAdded =
      opts.mode == LayoutMode::Organic
          ? organicSeed(work, split.interior, catalog, opts)
          : fishboneSeed(work, split.interior, catalog, opts);

  for (const auto& entry : split.peripheralRanked) {
    const ModuleDef* d = catalog.find(entry.second);
    if (placePeripheral(work, catalog, *d, classifyRole(*d), opts).has_value()) continue;
    // A dense interior can leave no seat whose neighbourhood clears a big dock body + corridor:
    // grow an escape arm (the seed's ladder) and retry, so the dock lands on the periphery at
    // the end of an arm instead of being dropped.
    bool placed = false;
    InstanceId arm = 0;
    for (int round = 0; round < 8; ++round) {  // mirrors the seed ladder's kMaxEscalation
      arm = injectStructureFor(work, catalog, *d, opts, result.report.connectorsAdded, arm);
      if (arm == 0) break;
      placed = placePeripheral(work, catalog, *d, classifyRole(*d), opts).has_value();
      if (placed) break;
    }
    // Truly unattachable (venture-tagged ports with no venture structure, or a corridor star
    // no in-plot pose clears): float it in-plot as a free-fall root; when even that fails the
    // reconciliation below reports it in skippedDefs instead of dropping it silently.
    if (!placed) floatInsidePlot(work, catalog, *d, opts);
  }

  result.station = work;
  result.placements = extractPlacements(work, existing, catalog);
  for (const LayoutPlacement& p : result.placements) {
    if (p.snapped)
      ++result.report.snapped;
    else
      ++result.report.floating;
  }
  std::unordered_map<std::string, int> requestedFunctional;
  for (const std::string& id : oc.placement) {
    const ModuleDef* d = catalog.find(id);
    if (d != nullptr && d->category != Category::Connector) ++requestedFunctional[id];
  }
  std::unordered_map<std::string, int> placedFunctional;
  for (const LayoutPlacement& p : result.placements) {
    const ModuleDef* d = catalog.find(p.defId);
    if (d != nullptr && d->category != Category::Connector) ++placedFunctional[p.defId];
  }
  int functionalPlaced = 0;
  for (const auto& e : placedFunctional) functionalPlaced += e.second;
  for (const auto& e : requestedFunctional)
    if (placedFunctional[e.first] < e.second) result.report.skippedDefs.push_back(e.first);
  sortUnique(result.report.skippedDefs);
  result.report.skipped = oc.requested - functionalPlaced;
  return result;
}

}  // namespace x4sb

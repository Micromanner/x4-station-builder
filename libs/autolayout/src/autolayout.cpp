#include "x4sb/autolayout/autolayout.hpp"

#include "x4sb/snap/snap.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace x4sb {
namespace {

int categoryPriority(Category c) {
  switch (c) {
    case Category::Connector: return 0;
    case Category::Production: return 1;
    case Category::Storage: return 2;
    case Category::Habitat: return 3;
    case Category::Defense: return 4;
    case Category::Other: return 5;
    case Category::Dock: return 6;
  }
  return 5;
}

double aabbVolume(const AABB& b) {
  const Vec3 d = b.max - b.min;
  return std::abs(d.x * d.y * d.z);
}

bool insidePlot(const AABB& box, double h) {
  return box.min.x >= -h && box.max.x <= h && box.min.y >= -h && box.max.y <= h &&
         box.min.z >= -h && box.max.z <= h;
}

Vec3 centroidOf(const Station& s) {
  if (s.empty()) return {};
  Vec3 sum{};
  for (const auto& m : s.modules()) sum = sum + m.worldTransform.position;
  return sum * (1.0 / static_cast<double>(s.size()));
}

AABB boundsOf(const Station& s, const ModuleCatalog& catalog) {
  AABB box{};
  bool any = false;
  for (const auto& m : s.modules()) {
    const ModuleDef* d = catalog.find(m.defId);
    if (d == nullptr) continue;
    const AABB wb = worldAabb(d->aabb, m.worldTransform);
    box = any ? merge(box, wb) : wb;
    any = true;
  }
  return box;
}

const ConnectionPoint* pointById(const ModuleDef& def, const std::string& id) {
  for (const auto& cp : def.connectionPoints)
    if (cp.id == id) return &cp;
  return nullptr;
}

void addFloating(Station& work, const ModuleDef& def, const Transform& xf, AutoLayoutResult& r) {
  PlacedModule m;
  m.defId = def.id;
  m.worldTransform = xf;
  const InstanceId id = work.add(m);
  ++r.report.floating;
  r.placements.push_back(LayoutPlacement{def.id, xf, false, 0, "", "", id});
}

void addSnapped(Station& work, const ModuleDef& def, const Transform& xf, InstanceId targetId,
                const std::string& newPt, const std::string& targetPt, AutoLayoutResult& r) {
  PlacedModule m;
  m.defId = def.id;
  m.worldTransform = xf;
  m.links.push_back(Link{newPt, targetId, targetPt});
  const InstanceId id = work.add(m);
  if (PlacedModule* t = work.find(targetId)) t->links.push_back(Link{targetPt, id, newPt});
  ++r.report.snapped;
  r.placements.push_back(LayoutPlacement{def.id, xf, true, targetId, newPt, targetPt, id});
}

struct Candidate {
  InstanceId targetId = 0;
  std::string targetPt;
  double dist = 0.0;    // connector distance from the station centroid
  bool sameCat = false;
};

// §4.3 — snap `def` onto a free compatible connector, same-category first then outward
// (farthest from centroid), accepting the first legal mate. Returns true on success.
bool trySnap(Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
             const AutoLayoutOptions& opts, AutoLayoutResult& r) {
  const Vec3 centroid = centroidOf(work);
  std::vector<Candidate> cands;
  for (const auto& placed : work.modules()) {
    const ModuleDef* od = catalog.find(placed.defId);
    if (od == nullptr) continue;
    for (const auto& cp : od->connectionPoints) {
      if (connectorIsLinked(placed, cp.id)) continue;
      bool compat = false;
      for (const auto& np : def.connectionPoints)
        if (connectorsCompatible(cp, np)) {
          compat = true;
          break;
        }
      if (!compat) continue;
      const Vec3 world = apply(placed.worldTransform, cp.localPosition);
      cands.push_back(Candidate{placed.instanceId, cp.id, length(world - centroid),
                                od->category == def.category});
    }
  }
  std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
    if (a.sameCat != b.sameCat) return a.sameCat;  // same-category preferred
    if (a.dist > b.dist) return true;              // outward growth (no float == compare)
    if (a.dist < b.dist) return false;
    if (a.targetId != b.targetId) return a.targetId < b.targetId;
    return a.targetPt < b.targetPt;
  });

  for (const Candidate& cand : cands) {
    const PlacedModule* tm = work.find(cand.targetId);
    if (tm == nullptr) continue;
    const ModuleDef* od = catalog.find(tm->defId);
    if (od == nullptr) continue;
    const ConnectionPoint* tp = pointById(*od, cand.targetPt);
    if (tp == nullptr) continue;
    for (const auto& np : def.connectionPoints) {
      if (!connectorsCompatible(*tp, np)) continue;
      const Transform xf =
          computeSnapTransform(work, catalog, cand.targetId, cand.targetPt, def, np.id);
      if (!insidePlot(worldAabb(def.aabb, xf), opts.plotHalfExtent)) continue;
      if (collidesWithStation(def, xf, cand.targetId, work, catalog)) continue;
      // Clearance corridors are checked against all modules (the snap partner's body
      // is NOT exempt): a dock whose corridor re-enters its mount target's body is
      // still an obstruction and must be rejected.
      if (collidesClearance(def, xf, 0, 0, work, catalog)) continue;
      addSnapped(work, def, xf, cand.targetId, np.id, cand.targetPt, r);
      return true;
    }
  }
  return false;
}

// §4.5 — next collision-free, in-plot floating slot on the outward XZ grid; nullopt if
// the plot is saturated. `cx`/`cz` advance across calls so successive floats spread out.
std::optional<Transform> nextFloatSlot(Station& work, const ModuleCatalog& catalog,
                                       const ModuleDef& def, const AutoLayoutOptions& opts,
                                       double& cx, double& cz) {
  const Vec3 span = def.aabb.max - def.aabb.min;
  const double stepZ = std::abs(span.z) + opts.floatingMargin;
  const double stepX = std::abs(span.x) + opts.floatingMargin;
  const double h = opts.plotHalfExtent;
  const Vec3 center{(def.aabb.min.x + def.aabb.max.x) * 0.5, (def.aabb.min.y + def.aabb.max.y) * 0.5,
                    (def.aabb.min.z + def.aabb.max.z) * 0.5};
  while (cx <= h) {
    Transform xf;
    xf.position = Vec3{cx, 0.0, cz} - center;  // module centre sits at (cx, 0, cz)
    const bool ok = insidePlot(worldAabb(def.aabb, xf), h) &&
                    !collidesWithStation(def, xf, 0, work, catalog) &&
                    !collidesClearance(def, xf, 0, 0, work, catalog);
    cz += stepZ;
    if (cz > h) {
      cz = -h;
      cx += stepX;
    }
    if (ok) return xf;
  }
  return std::nullopt;
}

}  // namespace

OrderedCart orderedPlacement(const QuantityList& cart, const ModuleCatalog& catalog) {
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
  std::sort(out.placement.begin(), out.placement.end(),
            [&](const std::string& a, const std::string& b) {
              const ModuleDef* da = catalog.find(a);  // non-null: only valid ids reach here
              const ModuleDef* db = catalog.find(b);
              const int pa = categoryPriority(da->category);
              const int pb = categoryPriority(db->category);
              if (pa != pb) return pa < pb;
              const double va = aabbVolume(da->aabb);
              const double vb = aabbVolume(db->aabb);
              if (va > vb) return true;   // larger first (no float == comparison)
              if (va < vb) return false;
              return a < b;
            });
  std::sort(out.skipped.begin(), out.skipped.end());
  out.skipped.erase(std::unique(out.skipped.begin(), out.skipped.end()), out.skipped.end());
  return out;
}

AutoLayoutResult autoLayout(const Station& existing, const QuantityList& cart,
                            const ModuleCatalog& catalog, const AutoLayoutOptions& opts) {
  AutoLayoutResult result;
  result.station = existing;  // work on a copy; `existing` is never mutated
  Station& work = result.station;

  const OrderedCart oc = orderedPlacement(cart, catalog);
  result.report.requested = oc.requested;
  result.report.skipped = oc.requested - static_cast<int>(oc.placement.size());
  result.report.skippedDefs = oc.skipped;

  const AABB bounds = boundsOf(work, catalog);
  double cx = (work.empty() ? 0.0 : bounds.max.x) + opts.floatingMargin;
  double cz = -opts.plotHalfExtent;

  for (const std::string& defId : oc.placement) {
    const ModuleDef* def = catalog.find(defId);  // non-null: orderedPlacement filtered
    if (def == nullptr) continue;

    if (work.empty()) {  // §4.2 greenfield seed at the origin
      addFloating(work, *def, Transform{}, result);
      continue;
    }
    if (trySnap(work, catalog, *def, opts, result)) continue;  // §4.3 snap-primary

    if (auto xf = nextFloatSlot(work, catalog, *def, opts, cx, cz)) {  // §4.5 fallback
      addFloating(work, *def, *xf, result);
    } else {
      ++result.report.skipped;  // plot saturated — reported, not placed out of bounds
      result.report.skippedDefs.push_back(defId);
    }
  }
  // One entry per distinct id: the saturation path appends a def once per overflow
  // instance, so dedupe to match the unknown/non-buildable prefix's shape.
  std::sort(result.report.skippedDefs.begin(), result.report.skippedDefs.end());
  result.report.skippedDefs.erase(
      std::unique(result.report.skippedDefs.begin(), result.report.skippedDefs.end()),
      result.report.skippedDefs.end());
  return result;
}

}  // namespace x4sb

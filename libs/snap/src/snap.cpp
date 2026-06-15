#include "x4sb/snap/snap.hpp"

namespace x4sb {
namespace {

const ConnectionPoint* findPoint(const ModuleDef& def, const std::string& id) {
  for (const auto& cp : def.connectionPoints)
    if (cp.id == id) return &cp;
  return nullptr;
}

bool pointIsLinked(const PlacedModule& m, const std::string& pointId) {
  for (const auto& l : m.links)
    if (l.thisPointId == pointId) return true;
  return false;
}

// Compatible if either side is untagged, or the tags match. The real
// compatibility rule comes from the component XML (spec §5/§10.3).
bool compatible(const ConnectionPoint& a, const ConnectionPoint& b) {
  return a.type.empty() || b.type.empty() || a.type == b.type;
}

}  // namespace

Transform computeSnapTransform(const Station& station, const ModuleCatalog& catalog,
                               InstanceId targetInstanceId, const std::string& targetPointId,
                               const ModuleDef& newDef, const std::string& newPointId) {
  // TODO(snap): full solve — orient newDef so its connector axis opposes the
  // target's (the 180° flip), then translate. Baseline keeps identity rotation
  // and only translates so the two points coincide; this already satisfies the
  // "points coincide" invariant and unblocks the rest of the pipeline.
  Transform out;
  const PlacedModule* target = station.find(targetInstanceId);
  if (!target) return out;
  const ModuleDef* targetDef = catalog.find(target->defId);
  if (!targetDef) return out;
  const ConnectionPoint* tp = findPoint(*targetDef, targetPointId);
  const ConnectionPoint* np = findPoint(newDef, newPointId);
  if (!tp || !np) return out;

  const Vec3 targetWorld = apply(target->worldTransform, tp->localPosition);
  out.rotation = Quat{};  // identity (baseline)
  out.position = targetWorld - rotate(out.rotation, np->localPosition);
  return out;
}

std::optional<SnapCandidate> findSnapCandidate(const ModuleDef& newDef, Vec3 cursorWorldPos,
                                               const Station& station, const ModuleCatalog& catalog,
                                               double radius) {
  std::optional<SnapCandidate> best;
  double bestDist = radius;

  for (const auto& placed : station.modules()) {
    const ModuleDef* def = catalog.find(placed.defId);
    if (!def) continue;
    for (const auto& cp : def->connectionPoints) {
      if (pointIsLinked(placed, cp.id)) continue;
      const Vec3 world = apply(placed.worldTransform, cp.localPosition);
      const double dist = length(world - cursorWorldPos);
      if (dist > bestDist) continue;
      for (const auto& np : newDef.connectionPoints) {
        if (!compatible(cp, np)) continue;
        bestDist = dist;
        best = SnapCandidate{placed.instanceId, cp.id, np.id};
        break;
      }
    }
  }
  return best;
}

bool collidesWithStation(const ModuleDef& def, const Transform& worldTransform,
                         InstanceId ignoreInstanceId, const Station& station,
                         const ModuleCatalog& catalog) {
  // TODO(snap): rotation-aware world AABB (or OBB per spec §6). Baseline only
  // translates the local AABB, which is exact while rotation is identity.
  const AABB a = def.aabb + worldTransform.position;
  for (const auto& placed : station.modules()) {
    if (placed.instanceId == ignoreInstanceId) continue;
    const ModuleDef* other = catalog.find(placed.defId);
    if (!other) continue;
    const AABB b = other->aabb + placed.worldTransform.position;
    if (overlaps(a, b)) return true;
  }
  return false;
}

}  // namespace x4sb

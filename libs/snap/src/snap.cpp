#include "x4sb/snap/snap.hpp"

#include "x4sb/document/commands.hpp"

#include <memory>

namespace x4sb {
namespace {

const ConnectionPoint* findPoint(const ModuleDef& def, const std::string& id) {
  for (const auto& cp : def.connectionPoints)
    if (cp.id == id) return &cp;
  return nullptr;
}

// Connector mate convention (spec §3.1 — CONFIRMED against real install assets via the
// editor's --snaptest harness: two struct_arg_cross_01 hubs mate into a clean,
// non-overlapping joint with coincident connectors and opposing normals, meshes aligned
// to their AABBs): two connectors mate by rotating the new module 180deg about local Y,
// which sends a module-local +Z connector normal to -Z so the normals oppose. Only this
// mate rotation is a code constant; the +Z normal axis is implicit in this choice (and
// probed by the snap tests). If a module family ever shows a different convention, change
// kMate here (and the tests' axis literals + render.cpp's connector-normal axis).
const Quat kMate{0.0, 0.0, 1.0, 0.0};  // 180deg about local Y (validated)

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
  Transform out;
  const PlacedModule* target = station.find(targetInstanceId);
  if (!target) return out;
  const ModuleDef* targetDef = catalog.find(target->defId);
  if (!targetDef) return out;
  const ConnectionPoint* tp = findPoint(*targetDef, targetPointId);
  const ConnectionPoint* np = findPoint(newDef, newPointId);
  if (!tp || !np) return out;

  // Approach A (spec §3): the new connector's world frame = the target connector's
  // world frame composed with the 180deg mate, so the two normals oppose. Then
  // translate so the connector points coincide.
  const Quat targetFrame = target->worldTransform.rotation * tp->localRotation;
  out.rotation = targetFrame * kMate * conjugate(np->localRotation);

  const Vec3 targetWorld = apply(target->worldTransform, tp->localPosition);
  out.position = targetWorld - rotate(out.rotation, np->localPosition);
  return out;
}

std::optional<SnapCandidate> findSnapCandidate(const ModuleDef& newDef, Vec3 cursorWorldPos,
                                               const Station& station, const ModuleCatalog& catalog,
                                               double radius, InstanceId ignoreInstanceId) {
  std::optional<SnapCandidate> best;
  double bestDist = radius;

  for (const auto& placed : station.modules()) {
    if (placed.instanceId == ignoreInstanceId) continue;  // snap-on-move: skip the dragged module
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
  const AABB a = worldAabb(def.aabb, worldTransform);
  for (const auto& placed : station.modules()) {
    if (placed.instanceId == ignoreInstanceId) continue;
    const ModuleDef* other = catalog.find(placed.defId);
    if (!other) continue;
    const AABB b = worldAabb(other->aabb, placed.worldTransform);
    if (overlaps(a, b)) return true;
  }
  return false;
}

std::unique_ptr<Command> makeSnapPlacement(const ModuleDef& newDef, Vec3 cursorWorldPos,
                                           const Station& station, const ModuleCatalog& catalog,
                                           double radius) {
  const std::optional<SnapCandidate> cand =
      findSnapCandidate(newDef, cursorWorldPos, station, catalog, radius);
  if (!cand) return nullptr;

  // Precondition: cand came from findSnapCandidate, so the target + both points
  // resolve; computeSnapTransform cannot hit its identity-on-miss fallback here.
  const Transform xf = computeSnapTransform(station, catalog, cand->instanceId,
                                            cand->targetPointId, newDef, cand->newPointId);
  // NOTE: only the joint partner is excluded from the collision test. In a dense
  // station the new module's conservative world-AABB may overlap an already-placed
  // neighbour of the target and be rejected here. Acceptable for manual placement;
  // auto-layout / tight chains may need OBB-precise collision (spec §6) or a
  // multi-id ignore set. TODO(snap): revisit when auto-layout lands.
  if (collidesWithStation(newDef, xf, cand->instanceId, station, catalog)) return nullptr;

  return std::make_unique<PlaceModuleCommand>(newDef.id, xf, cand->instanceId, cand->newPointId,
                                              cand->targetPointId);  // (..., newPointId, targetPointId)
}

}  // namespace x4sb

#include "x4sb/snap/snap.hpp"

#include "x4sb/document/commands.hpp"

#include <cmath>
#include <memory>
#include <string_view>

namespace x4sb {
namespace {

const ConnectionPoint* findPoint(const ModuleDef& def, const std::string& id) {
  for (const auto& cp : def.connectionPoints)
    if (cp.id == id) return &cp;
  return nullptr;
}

// A local clearance volume placed into the world by a module's transform.
Obb worldObb(const ClearanceVolume& v, const Transform& xf) {
  return Obb{apply(xf, v.center), xf.rotation * v.rotation, v.halfExtents};
}

// A module's body as an oriented box in world space: its local AABB carried rigidly
// by the placement transform. The narrow-phase counterpart of worldAabb — tighter,
// because it does not inflate a rotated box by up to sqrt(2)x (spec §6).
Obb worldBodyObb(const AABB& local, const Transform& xf) {
  const Vec3 center{(local.min.x + local.max.x) * 0.5, (local.min.y + local.max.y) * 0.5,
                    (local.min.z + local.max.z) * 0.5};
  const Vec3 half{(local.max.x - local.min.x) * 0.5, (local.max.y - local.min.y) * 0.5,
                  (local.max.z - local.min.z) * 0.5};
  return Obb{apply(xf, center), xf.rotation, half};
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

// True if the whitespace-separated tag string contains the exact token.
bool hasTag(std::string_view tags, std::string_view token) {
  std::size_t i = 0;
  while (i <= tags.size()) {
    const std::size_t e = tags.find(' ', i);
    const std::string_view t =
        tags.substr(i, e == std::string_view::npos ? std::string_view::npos : e - i);
    if (t == token) return true;
    if (e == std::string_view::npos) break;
    i = e + 1;
  }
  return false;
}

}  // namespace

// Token-based, calibrated against real construction plans (in-game ground truth, 2026-07-03):
// the game links "snap" to "snap snap_aligned" (user plans: vertical structs onto crosses,
// piers, habitats) and to "snap ter_energycells" (vanilla ter_solarpowerplant plan), so
// auxiliary tags do NOT gate mating — exact-string equality made those module classes
// unplaceable (known-issues 4.3). "venture" stays discriminating BOTH ways: no vanilla plan
// mates venture to plain snap, and loosening it re-opens the venture-trap (venturer hubs
// synthesized as hosts that nothing in-game accepts). Empty stays a wildcard for synthetic
// fixtures — every real catalog port carries "snap" (isSnapConnection filters on it).
bool connectorsCompatible(const ConnectionPoint& a, const ConnectionPoint& b) {
  if (a.type.empty() || b.type.empty()) return true;
  return hasTag(a.type, "snap") && hasTag(b.type, "snap") &&
         hasTag(a.type, "venture") == hasTag(b.type, "venture");
}

bool connectorIsLinked(const PlacedModule& m, const std::string& pointId) {
  for (const auto& l : m.links)
    if (l.thisPointId == pointId) return true;
  return false;
}

Transform computeSnapTransform(const Station& station, const ModuleCatalog& catalog,
                               InstanceId targetInstanceId, const std::string& targetPointId,
                               const ModuleDef& newDef, const std::string& newPointId,
                               double clockingRad) {
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
  // Insert a roll about the connector's local +Z normal between the target frame and
  // kMate: it rotates the new module around the shared axis without disturbing the
  // coincident point or the opposing-normals invariant. clockingRad = 0 -> kMate alone.
  const Quat roll = axisAngle(Vec3{0, 0, 1}, clockingRad);
  out.rotation = targetFrame * roll * kMate * conjugate(np->localRotation);

  const Vec3 targetWorld = apply(target->worldTransform, tp->localPosition);
  out.position = targetWorld - rotate(out.rotation, np->localPosition);
  return out;
}

std::optional<MateChoice> chooseMate(const Station& station, const ModuleCatalog& catalog,
                                     InstanceId targetInstanceId, const std::string& targetPointId,
                                     const ModuleDef& newDef, const Quat& desiredWorldRot) {
  const PlacedModule* target = station.find(targetInstanceId);
  if (!target) return std::nullopt;
  const ModuleDef* targetDef = catalog.find(target->defId);
  if (!targetDef) return std::nullopt;
  const ConnectionPoint* tp = findPoint(*targetDef, targetPointId);
  if (!tp) return std::nullopt;

  const Quat targetFrame = target->worldTransform.rotation * tp->localRotation;
  const Vec3 normal = rotate(targetFrame, Vec3{0, 0, 1});  // shared connector world axis

  std::optional<MateChoice> best;
  double bestErr = 0.0;
  for (const ConnectionPoint& np : newDef.connectionPoints) {
    if (!connectorsCompatible(*tp, np)) continue;
    const Transform base =
        computeSnapTransform(station, catalog, targetInstanceId, targetPointId, newDef, np.id, 0.0);
    const double clocking = twistAngle(desiredWorldRot * conjugate(base.rotation), normal);
    const Transform xf = computeSnapTransform(station, catalog, targetInstanceId, targetPointId,
                                              newDef, np.id, clocking);
    const Quat& r = xf.rotation;
    const double d = std::abs(r.w * desiredWorldRot.w + r.x * desiredWorldRot.x +
                              r.y * desiredWorldRot.y + r.z * desiredWorldRot.z);
    const double err = 1.0 - d;            // 0 == exact match
    if (!best || err < bestErr - 1e-12) {  // strict improvement -> ties keep lower index
      best = MateChoice{np.id, clocking, xf};
      bestErr = err;
    }
  }
  return best;
}

std::optional<SnapCandidate> findSnapCandidate(const ModuleDef& newDef, Vec3 cursorWorldPos,
                                               const Station& station, const ModuleCatalog& catalog,
                                               double radius, InstanceId ignoreInstanceId,
                                               std::optional<Transform> newDefTransform) {
  std::optional<SnapCandidate> best;
  double bestDist = radius;

  for (const auto& placed : station.modules()) {
    if (placed.instanceId == ignoreInstanceId) continue;  // snap-on-move: skip the dragged module
    const ModuleDef* def = catalog.find(placed.defId);
    if (!def) continue;
    for (const auto& cp : def->connectionPoints) {
      if (connectorIsLinked(placed, cp.id)) continue;
      const Vec3 world = apply(placed.worldTransform, cp.localPosition);

      if (newDefTransform) {
        // Connector-to-connector distance comparison for dragging/snap-on-move
        for (const auto& np : newDef.connectionPoints) {
          if (!connectorsCompatible(cp, np)) continue;
          const Vec3 newWorld = apply(*newDefTransform, np.localPosition);
          const double dist = length(world - newWorld);
          if (dist < bestDist) {
            bestDist = dist;
            best = SnapCandidate{placed.instanceId, cp.id, np.id};
          }
        }
      } else {
        // Traditional cursor-based distance comparison
        const double dist = length(world - cursorWorldPos);
        if (dist > bestDist) continue;
        for (const auto& np : newDef.connectionPoints) {
          if (!connectorsCompatible(cp, np)) continue;
          bestDist = dist;
          best = SnapCandidate{placed.instanceId, cp.id, np.id};
          break;
        }
      }
    }
  }
  return best;
}

std::optional<SnapCandidate> findSnapCandidate(const ModuleDef& newDef, Vec3 queryPoint,
                                               const Station& station, const ModuleCatalog& catalog,
                                               const ConnectorGrid& grid, double radius,
                                               InstanceId ignoreInstanceId,
                                               std::optional<Transform> newDefTransform) {
  // Equivalence to the brute-force findSnapCandidate is "same nearest distance", not a
  // byte-identical result: on an EXACT distance tie the grid (cell-iteration order) and
  // the brute force (station order) can pick different but equally-valid candidates.
  // Ties are measure-zero on real float poses and unspecified by spec §6.
  std::optional<SnapCandidate> best;
  double bestDist = radius;

  // Resolve a grid entry to its (placed module, connection point), applying the
  // same skips as the brute-force search. Returns false if it should be ignored.
  const auto resolve = [&](const ConnectorGrid::Entry& e, const PlacedModule*& placedOut,
                           const ConnectionPoint*& cpOut) -> bool {
    if (e.instanceId == ignoreInstanceId) return false;
    const PlacedModule* placed = station.find(e.instanceId);
    if (placed == nullptr) return false;
    const ModuleDef* def = catalog.find(placed->defId);
    if (def == nullptr || e.connectorIndex >= def->connectionPoints.size()) return false;
    const ConnectionPoint& cp = def->connectionPoints[e.connectorIndex];
    if (connectorIsLinked(*placed, cp.id)) return false;
    placedOut = placed;
    cpOut = &cp;
    return true;
  };

  if (newDefTransform) {
    // Snap-on-move: nearest compatible connector-to-connector pair. Query the grid
    // around each of the new module's connectors (N is small).
    for (const auto& np : newDef.connectionPoints) {
      const Vec3 newWorld = apply(*newDefTransform, np.localPosition);
      for (const ConnectorGrid::Entry& e : grid.queryRadius(newWorld, radius)) {
        const PlacedModule* placed = nullptr;
        const ConnectionPoint* cp = nullptr;
        if (!resolve(e, placed, cp)) continue;
        if (!connectorsCompatible(*cp, np)) continue;
        const double dist = length(e.world - newWorld);
        if (dist < bestDist) {
          bestDist = dist;
          best = SnapCandidate{placed->instanceId, cp->id, np.id};
        }
      }
    }
    return best;
  }

  // Cursor mode: nearest target connector to the cursor that has any compatible
  // connector on the new module.
  for (const ConnectorGrid::Entry& e : grid.queryRadius(queryPoint, radius)) {
    const PlacedModule* placed = nullptr;
    const ConnectionPoint* cp = nullptr;
    if (!resolve(e, placed, cp)) continue;
    const double dist = length(e.world - queryPoint);
    if (dist > bestDist) continue;
    for (const auto& np : newDef.connectionPoints) {
      if (!connectorsCompatible(*cp, np)) continue;
      bestDist = dist;
      best = SnapCandidate{placed->instanceId, cp->id, np.id};
      break;
    }
  }
  return best;
}

bool collidesWithStation(const ModuleDef& def, const Transform& worldTransform, InstanceId ignoreA,
                         InstanceId ignoreB, const Station& station, const ModuleCatalog& catalog) {
  const AABB a = worldAabb(def.aabb, worldTransform);
  const Obb aObb = worldBodyObb(def.aabb, worldTransform);
  for (const auto& placed : station.modules()) {
    if (placed.instanceId == ignoreA || placed.instanceId == ignoreB) continue;
    const ModuleDef* other = catalog.find(placed.defId);
    if (!other) continue;
    // Broad phase: cheap conservative AABB reject. If the inflated hulls miss, the
    // tight bodies certainly miss, so the narrow phase is unnecessary (keeps the snap
    // hot path fast).
    if (!overlaps(a, worldAabb(other->aabb, placed.worldTransform))) continue;
    // Narrow phase: OBB-precise (spec §6). Removes the false overlaps that the
    // sqrt(2)-inflated AABBs of rotated neighbours produce, so auto-layout can pack
    // modules tightly around a hub's side ports instead of spawning extra hubs.
    if (overlapsObbObb(aObb, worldBodyObb(other->aabb, placed.worldTransform))) return true;
  }
  return false;
}

bool collidesWithStation(const ModuleDef& def, const Transform& worldTransform,
                         InstanceId ignoreInstanceId, const Station& station,
                         const ModuleCatalog& catalog) {
  return collidesWithStation(def, worldTransform, ignoreInstanceId, 0, station, catalog);
}

bool collidesClearance(const ModuleDef& def, const Transform& worldTransform, InstanceId ignoreA,
                       InstanceId ignoreB, const Station& station, const ModuleCatalog& catalog) {
  const AABB myBody = worldAabb(def.aabb, worldTransform);
  for (const auto& placed : station.modules()) {
    if (placed.instanceId == ignoreA || placed.instanceId == ignoreB) continue;
    const ModuleDef* other = catalog.find(placed.defId);
    if (!other) continue;

    // My corridors vs their body.
    const AABB otherBody = worldAabb(other->aabb, placed.worldTransform);
    for (const ClearanceVolume& cv : def.clearanceVolumes)
      if (overlapsObbAabb(worldObb(cv, worldTransform), otherBody)) return true;

    // Their corridors vs my body.
    for (const ClearanceVolume& cv : other->clearanceVolumes)
      if (overlapsObbAabb(worldObb(cv, placed.worldTransform), myBody)) return true;
  }
  return false;
}

std::vector<ClearanceHit> violatedClearance(const ModuleDef& def, const Transform& worldTransform,
                                            InstanceId ignoreA, InstanceId ignoreB,
                                            const Station& station, const ModuleCatalog& catalog) {
  std::vector<ClearanceHit> hits;
  const AABB myBody = worldAabb(def.aabb, worldTransform);
  for (const auto& placed : station.modules()) {
    if (placed.instanceId == ignoreA || placed.instanceId == ignoreB) continue;
    const ModuleDef* other = catalog.find(placed.defId);
    if (!other) continue;
    for (std::size_t i = 0; i < other->clearanceVolumes.size(); ++i)
      if (overlapsObbAabb(worldObb(other->clearanceVolumes[i], placed.worldTransform), myBody))
        hits.push_back({placed.instanceId, i});
  }
  return hits;
}

std::unique_ptr<Command> makeSnapPlacement(const ModuleDef& newDef, Vec3 cursorWorldPos,
                                           const Station& station, const ModuleCatalog& catalog,
                                           double radius) {
  const std::optional<SnapCandidate> cand =
      findSnapCandidate(newDef, cursorWorldPos, station, catalog, radius);
  if (!cand) return nullptr;

  // Precondition: cand came from findSnapCandidate, so the target + both points
  // resolve; computeSnapTransform cannot hit its identity-on-miss fallback here.
  const Transform xf = computeSnapTransform(station, catalog, cand->instanceId, cand->targetPointId,
                                            newDef, cand->newPointId);
  // NOTE: only the joint partner is excluded from the collision test. In a dense
  // station the new module's conservative world-AABB may overlap an already-placed
  // neighbour of the target and be rejected here. Acceptable for manual placement;
  // auto-layout / tight chains may need OBB-precise collision (spec §6) or a
  // multi-id ignore set. TODO(snap): revisit when auto-layout lands.
  if (collidesWithStation(newDef, xf, cand->instanceId, station, catalog)) return nullptr;

  return std::make_unique<PlaceModuleCommand>(
      newDef.id, xf, cand->instanceId, cand->newPointId,
      cand->targetPointId);  // (..., newPointId, targetPointId)
}

RollResult rollAboutConnector(const Transform& startTransform, Vec3 gizmoAxisWorld, double radians,
                              Vec3 connectorWorldPos, bool hasSingleJoint) {
  // A singly-connected module pivots about its connector point for any axis, so the joint
  // point stays fixed and the body hinges around where it's attached (stays mated). Zero or
  // 2+ joints have no single pivot, so they free-rotate about the start position (detach).
  if (hasSingleJoint) {
    return {rotateAbout(startTransform, connectorWorldPos, gizmoAxisWorld, radians), true};
  }
  Transform freePose = startTransform;
  freePose.rotation = axisAngle(gizmoAxisWorld, radians) * startTransform.rotation;
  return {freePose, false};
}

}  // namespace x4sb

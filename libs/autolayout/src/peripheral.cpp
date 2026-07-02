#include "x4sb/autolayout/peripheral.hpp"

#include "x4sb/autolayout/roles.hpp"
#include "x4sb/snap/snap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace x4sb {
namespace {

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

Vec3 normalizeOr(Vec3 v, Vec3 fallback) {
  const double len = length(v);
  if (len < 1e-9) return fallback;
  return v * (1.0 / len);
}

// Unit quaternion rotating unit vector `from` onto unit vector `to`.
Quat fromTo(Vec3 from, Vec3 to) {
  const double c = dot(from, to);
  if (c > 1.0 - 1e-9) return {};  // already aligned
  if (c < -1.0 + 1e-9) {          // opposite: 180° about any perpendicular axis
    Vec3 axis = cross(from, Vec3{1, 0, 0});
    if (length(axis) < 1e-9) axis = cross(from, Vec3{0, 1, 0});
    const double pi = std::acos(-1.0);
    return axisAngle(axis, pi);
  }
  return axisAngle(cross(from, to), std::acos(std::clamp(c, -1.0, 1.0)));
}

// The synthesized forward clear box for a turret/solar mounted at `mateWorld` whose
// body lies at `xf`: an OBB starting at the body's outward face and extending outward
// along `outDir`. True if it is unobstructed by every body except `ignore` (the mount
// joint). Spec §10 — turrets/solar need open space ahead; docks use real corridors.
bool forwardClear(const ModuleDef& def, const Transform& xf, Vec3 mateWorld, Vec3 outDir,
                  InstanceId ignore, const Station& station, const ModuleCatalog& catalog) {
  const AABB body = worldAabb(def.aabb, xf);
  // Outward reach: how far the body sticks out along outDir past the mount point.
  const std::array<Vec3, 2> ext{{body.min, body.max}};
  double reach = 0.0;
  for (int i = 0; i < 8; ++i) {
    const Vec3 corner{ext[(i >> 0) & 1].x, ext[(i >> 1) & 1].y, ext[(i >> 2) & 1].z};
    reach = std::max(reach, dot(corner - mateWorld, outDir));
  }
  const Vec3 span = def.aabb.max - def.aabb.min;
  const double crossHalf = 0.5 * std::max({std::abs(span.x), std::abs(span.y), std::abs(span.z)});
  const double clearLen = std::max(crossHalf * 4.0, 50.0);  // a generous "fire lane"

  Obb fan;
  fan.rotation = fromTo(Vec3{1, 0, 0}, outDir);  // local +X maps to outDir
  fan.center = mateWorld + outDir * (reach + clearLen * 0.5);
  fan.halfExtents = Vec3{clearLen * 0.5, crossHalf, crossHalf};

  for (const auto& m : station.modules()) {
    if (m.instanceId == ignore) continue;
    const ModuleDef* md = catalog.find(m.defId);
    if (md == nullptr) continue;
    if (overlapsObbAabb(fan, worldAabb(md->aabb, m.worldTransform))) return false;
  }
  return true;
}

struct Cand {
  InstanceId targetId = 0;
  std::string targetPt;
  Vec3 mateWorld{};
  double dist = 0.0;              // mate distance from centroid (outermost first)
  bool targetPeripheral = false;  // target is itself a dock/turret/solar (avoid daisy-chaining)
};

}  // namespace

std::optional<LayoutPlacement> placePeripheral(Station& work, const ModuleCatalog& catalog,
                                               const ModuleDef& def, Role role,
                                               const AutoLayoutOptions& opts) {
  if (work.empty()) return std::nullopt;
  const Vec3 centroid = centroidOf(work);

  std::vector<Cand> cands;
  for (const auto& placed : work.modules()) {
    const ModuleDef* od = catalog.find(placed.defId);
    if (od == nullptr) continue;
    const bool tPeri = isPeripheral(classifyRole(*od));
    for (const auto& cp : od->connectionPoints) {
      if (connectorIsLinked(placed, cp.id)) continue;
      bool compat = false;
      for (const auto& np : def.connectionPoints)
        if (connectorsCompatible(cp, np)) {
          compat = true;
          break;
        }
      if (!compat) continue;
      const Vec3 mate = apply(placed.worldTransform, cp.localPosition);
      cands.push_back(Cand{placed.instanceId, cp.id, mate, length(mate - centroid), tPeri});
    }
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    // Attach to the main structure, not another peripheral — otherwise docks/turrets/solar
    // daisy-chain into a line that marches out of the plot (spec §6 outer-hull intent).
    if (a.targetPeripheral != b.targetPeripheral) return !a.targetPeripheral;
    if (a.dist > b.dist) return true;  // then outermost first (no float == compare)
    if (a.dist < b.dist) return false;
    if (a.targetId != b.targetId) return a.targetId < b.targetId;
    return a.targetPt < b.targetPt;
  });

  for (const Cand& cand : cands) {
    const PlacedModule* tm = work.find(cand.targetId);
    if (tm == nullptr) continue;
    const ModuleDef* od = catalog.find(tm->defId);
    if (od == nullptr) continue;
    const ConnectionPoint* tp = nullptr;
    for (const auto& cp : od->connectionPoints)
      if (cp.id == cand.targetPt) {
        tp = &cp;
        break;
      }
    if (tp == nullptr) continue;
    for (const auto& np : def.connectionPoints) {
      if (!connectorsCompatible(*tp, np)) continue;
      const Transform xf =
          computeSnapTransform(work, catalog, cand.targetId, cand.targetPt, def, np.id);
      if (!insidePlot(worldAabb(def.aabb, xf), opts.plotHalfExtent)) continue;
      if (collidesWithStation(def, xf, cand.targetId, work, catalog)) continue;
      if (collidesClearance(def, xf, 0, 0, work, catalog)) continue;
      // Match layoutLegal's conservative body test (the AABB overlap, not the OBB narrow-phase, is
      // what defines a legal plan): reject if this body's world-AABB meets any non-adjacent body,
      // so a peripheral never lands the whole plan in an "illegal overlap" state.
      const AABB myBox = worldAabb(def.aabb, xf);
      bool aabbClash = false;
      for (const auto& o : work.modules()) {
        if (o.instanceId == cand.targetId) continue;
        const ModuleDef* od2 = catalog.find(o.defId);
        if (od2 != nullptr && overlaps(myBox, worldAabb(od2->aabb, o.worldTransform))) {
          aabbClash = true;
          break;
        }
      }
      if (aabbClash) continue;
      // Outward direction: body center away from the centroid. Docks already had their
      // corridors checked by collidesClearance; turrets/solar need the forward lane clear.
      const Vec3 outDir = normalizeOr(xf.position - centroid, Vec3{1, 0, 0});
      if (role != Role::Dock &&
          !forwardClear(def, xf, cand.mateWorld, outDir, cand.targetId, work, catalog)) {
        continue;
      }

      PlacedModule m;
      m.defId = def.id;
      m.worldTransform = xf;
      m.links.push_back(Link{np.id, cand.targetId, cand.targetPt});
      const InstanceId id = work.add(m);
      if (PlacedModule* t = work.find(cand.targetId))
        t->links.push_back(Link{cand.targetPt, id, np.id});
      return LayoutPlacement{def.id, xf, true, cand.targetId, np.id, cand.targetPt, id};
    }
  }
  return std::nullopt;
}

}  // namespace x4sb

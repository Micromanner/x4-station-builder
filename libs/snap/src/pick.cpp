#include "x4sb/snap/pick.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace x4sb {

std::optional<double> rayIntersectsAabb(Vec3 origin, Vec3 dir, const AABB& box) {
  const std::array<double, 3> o{origin.x, origin.y, origin.z};
  const std::array<double, 3> d{dir.x, dir.y, dir.z};
  const std::array<double, 3> lo{box.min.x, box.min.y, box.min.z};
  const std::array<double, 3> hi{box.max.x, box.max.y, box.max.z};

  // A direction component below this magnitude is treated as parallel to the slab.
  constexpr double kParallelEpsilon = 1e-12;

  double tmin = -std::numeric_limits<double>::infinity();
  double tmax = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < 3; ++i) {
    if (std::abs(d[i]) < kParallelEpsilon) {
      if (o[i] < lo[i] || o[i] > hi[i]) return std::nullopt;  // parallel & outside the slab
      continue;
    }
    double t1 = (lo[i] - o[i]) / d[i];
    double t2 = (hi[i] - o[i]) / d[i];
    if (t1 > t2) std::swap(t1, t2);
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return std::nullopt;
  }
  if (tmax < 0) return std::nullopt;  // box entirely behind the ray
  return tmin >= 0 ? tmin : tmax;     // tmin < 0 -> origin inside box -> exit distance
}

std::optional<InstanceId> pickModule(const Station& station, const ModuleCatalog& catalog,
                                     Vec3 rayOrigin, Vec3 rayDir) {
  std::optional<InstanceId> best;
  double bestT = std::numeric_limits<double>::infinity();
  for (const auto& placed : station.modules()) {
    const ModuleDef* def = catalog.find(placed.defId);
    if (!def) continue;
    const AABB world = worldAabb(def->aabb, placed.worldTransform);
    const std::optional<double> t = rayIntersectsAabb(rayOrigin, rayDir, world);
    if (t && *t < bestT) {
      bestT = *t;
      best = placed.instanceId;
    }
  }
  return best;
}

}  // namespace x4sb

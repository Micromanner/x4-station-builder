#include "x4sb/autolayout/metrics.hpp"

namespace x4sb {
namespace {

Vec3 bodyCenter(const PlacedModule& m, const ModuleDef& d) {
  const Vec3 localCenter = (d.aabb.min + d.aabb.max) * 0.5;
  return apply(m.worldTransform, localCenter);
}

}  // namespace

Vec3 centroid(const Station& station, const ModuleCatalog& catalog) {
  Vec3 sum{};
  std::size_t n = 0;
  for (const auto& m : station.modules()) {
    const ModuleDef* d = catalog.find(m.defId);
    if (d == nullptr) continue;
    sum = sum + bodyCenter(m, *d);
    ++n;
  }
  if (n == 0) return {};
  return sum * (1.0 / static_cast<double>(n));
}

double radiusOfGyrationSq(const Station& station, const ModuleCatalog& catalog) {
  const Vec3 ctr = centroid(station, catalog);
  double sum = 0.0;
  std::size_t n = 0;
  for (const auto& m : station.modules()) {
    const ModuleDef* d = catalog.find(m.defId);
    if (d == nullptr) continue;
    const Vec3 delta = bodyCenter(m, *d) - ctr;
    sum += dot(delta, delta);
    ++n;
  }
  if (n == 0) return 0.0;
  return sum / static_cast<double>(n);
}

AABB bounds(const Station& station, const ModuleCatalog& catalog) {
  AABB box{};
  bool any = false;
  for (const auto& m : station.modules()) {
    const ModuleDef* d = catalog.find(m.defId);
    if (d == nullptr) continue;
    const AABB wb = worldAabb(d->aabb, m.worldTransform);
    box = any ? merge(box, wb) : wb;
    any = true;
  }
  return box;
}

}  // namespace x4sb

#include "x4sb/autolayout/objective.hpp"

#include "x4sb/autolayout/autolayout.hpp"  // AutoLayoutOptions (full definition)
#include "x4sb/autolayout/metrics.hpp"
#include "x4sb/data/math.hpp"

#include <cmath>

namespace x4sb {
namespace {

double aabbVolume(const AABB& b) {
  const Vec3 d = b.max - b.min;
  return std::abs(d.x * d.y * d.z);
}

}  // namespace

double connectorMaterial(const Station& station, const ModuleCatalog& catalog) {
  double material = 0.0;
  for (const auto& m : station.modules()) {
    const ModuleDef* d = catalog.find(m.defId);
    if (d == nullptr || d->category != Category::Connector) continue;
    material += 1.0 + aabbVolume(d->aabb);  // one piece + its volume (spec §8.1)
  }
  return material;
}

Objective compactObjective(const AutoLayoutOptions& opts) {
  const double wMaterial = opts.wMaterial;
  return [wMaterial](const Station& s, const ModuleCatalog& c) {
    return radiusOfGyrationSq(s, c) + wMaterial * connectorMaterial(s, c);
  };
}

}  // namespace x4sb

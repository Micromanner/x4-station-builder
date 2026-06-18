#include "x4sb/snap/connector_grid.hpp"

#include <cmath>

namespace x4sb {

std::size_t ConnectorGrid::CellKeyHash::operator()(const CellKey& k) const noexcept {
  // Classic spatial hash: per-axis large primes, xored. Unsigned wrap is defined.
  constexpr std::uint64_t p1 = 73856093U, p2 = 19349663U, p3 = 83492791U;
  const std::uint64_t h = static_cast<std::uint64_t>(k.x) * p1 ^
                          static_cast<std::uint64_t>(k.y) * p2 ^
                          static_cast<std::uint64_t>(k.z) * p3;
  return static_cast<std::size_t>(h);
}

ConnectorGrid::CellKey ConnectorGrid::cellOf(Vec3 p) const {
  return {static_cast<std::int64_t>(std::floor(p.x / cellSize_)),
          static_cast<std::int64_t>(std::floor(p.y / cellSize_)),
          static_cast<std::int64_t>(std::floor(p.z / cellSize_))};
}

ConnectorGrid::ConnectorGrid(const Station& station, const ModuleCatalog& catalog, double cellSize)
    : cellSize_(cellSize > 0.0 ? cellSize : 1.0) {
  for (const PlacedModule& pm : station.modules()) {
    const ModuleDef* def = catalog.find(pm.defId);
    if (def == nullptr) continue;
    for (std::size_t i = 0; i < def->connectionPoints.size(); ++i) {
      const Vec3 world = apply(pm.worldTransform, def->connectionPoints[i].localPosition);
      cells_[cellOf(world)].push_back(Entry{pm.instanceId, i, world});
      ++count_;
    }
  }
}

std::vector<ConnectorGrid::Entry> ConnectorGrid::queryRadius(Vec3 point, double radius) const {
  std::vector<Entry> out;
  if (radius < 0.0) return out;
  const CellKey lo = cellOf(point - Vec3{radius, radius, radius});
  const CellKey hi = cellOf(point + Vec3{radius, radius, radius});
  const double r2 = radius * radius;
  for (std::int64_t cx = lo.x; cx <= hi.x; ++cx)
    for (std::int64_t cy = lo.y; cy <= hi.y; ++cy)
      for (std::int64_t cz = lo.z; cz <= hi.z; ++cz) {
        const auto it = cells_.find(CellKey{cx, cy, cz});
        if (it == cells_.end()) continue;
        for (const Entry& e : it->second) {
          const Vec3 d = e.world - point;
          if (dot(d, d) <= r2) out.push_back(e);
        }
      }
  return out;
}

}  // namespace x4sb

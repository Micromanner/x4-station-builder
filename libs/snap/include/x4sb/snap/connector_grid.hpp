#pragma once
// Broad-phase spatial hash of placed-module connector world positions, so snap
// search and connector rendering touch only connectors near a query point
// instead of every connector in the station (known-issues 1.2). Pure geometry,
// no rendering. Rebuilt when the station mutates, never per frame.
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/document/station.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace x4sb {

class ConnectorGrid {
 public:
  // One stored connector: which placed module, which connector index on its def,
  // and the connector's world position (cached so a query needn't recompute it).
  // The caller resolves the ConnectionPoint via station/catalog for id/type/link.
  struct Entry {
    InstanceId instanceId{0};
    std::size_t connectorIndex{0};
    Vec3 world{};
  };

  // Build over every connector of every placed module. `cellSize` is the query
  // radius the grid is tuned for (a query then visits a small fixed cell
  // neighborhood); values <= 0 fall back to 1.0. Modules whose def is missing
  // from the catalog are skipped, matching the brute-force search.
  ConnectorGrid(const Station& station, const ModuleCatalog& catalog, double cellSize);

  // Every stored connector whose world position is within `radius` of `point`.
  // Visits only the cells overlapping point +/- radius, so cost is proportional
  // to local connector density, not the station's total connector count.
  [[nodiscard]] std::vector<Entry> queryRadius(Vec3 point, double radius) const;

  [[nodiscard]] std::size_t size() const { return count_; }

 private:
  struct CellKey {
    std::int64_t x{0}, y{0}, z{0};
    bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct CellKeyHash {
    std::size_t operator()(const CellKey& k) const noexcept;
  };

  [[nodiscard]] CellKey cellOf(Vec3 p) const;

  double cellSize_{1.0};
  std::size_t count_{0};
  std::unordered_map<CellKey, std::vector<Entry>, CellKeyHash> cells_;
};

}  // namespace x4sb

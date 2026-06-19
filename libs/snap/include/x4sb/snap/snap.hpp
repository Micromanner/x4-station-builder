#pragma once
// Snap engine — the core geometric primitive shared by manual and auto modes
// (spec §6). Pure geometry; no rendering.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"
#include "x4sb/snap/connector_grid.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace x4sb {

// Rigid-body transform placing newDef's `newPointId` exactly onto the target
// instance's `targetPointId`, rotated 180° about the connector axis so the two
// connectors face each other. Consequence: a directly-connected joint is
// collision-free by construction. Returns an identity Transform if the target
// instance or either connection point cannot be resolved.
Transform computeSnapTransform(const Station& station, const ModuleCatalog& catalog,
                               InstanceId targetInstanceId, const std::string& targetPointId,
                               const ModuleDef& newDef, const std::string& newPointId);

// Connector-mate predicates, the single source of truth for "which connectors may
// join" — shared by the snap search and the editor's snap guide-lines so the two
// never disagree. Compatible = either side untagged or the tags match (the real rule
// is XML-driven, spec §5/§10.3); linked = the module already has a link on that point.
[[nodiscard]] bool connectorsCompatible(const ConnectionPoint& a, const ConnectionPoint& b);
[[nodiscard]] bool connectorIsLinked(const PlacedModule& m, const std::string& pointId);

struct SnapCandidate {
  InstanceId instanceId{0};
  std::string targetPointId;
  std::string newPointId;
};

// Nearest free, compatible connection point within `radius`, or nullopt.
// `ignoreInstanceId` (default 0 = none) is skipped — used by snap-on-move so the
// dragged module is never matched against its own connectors.
std::optional<SnapCandidate> findSnapCandidate(const ModuleDef& newDef, Vec3 cursorWorldPos,
                                               const Station& station, const ModuleCatalog& catalog,
                                               double radius, InstanceId ignoreInstanceId = 0,
                                               std::optional<Transform> newDefTransform = std::nullopt);

// Grid-accelerated equivalent of findSnapCandidate: identical result, but uses a
// prebuilt ConnectorGrid to consider only connectors near the query point instead
// of scanning every placed module (known-issues 1.2). `grid` must have been built
// from the same `station`/`catalog`. `queryPoint` is the cursor world position
// (cursor mode) or the dragged pose's position (snap-on-move; pass newDefTransform).
[[nodiscard]] std::optional<SnapCandidate> findSnapCandidate(
    const ModuleDef& newDef, Vec3 queryPoint, const Station& station,
    const ModuleCatalog& catalog, const ConnectorGrid& grid, double radius,
    InstanceId ignoreInstanceId = 0, std::optional<Transform> newDefTransform = std::nullopt);

// AABB overlap test of a candidate placement against all OTHER placed modules
// (the joint itself cannot overlap). `ignoreInstanceId` is skipped.
bool collidesWithStation(const ModuleDef& def, const Transform& worldTransform,
                         InstanceId ignoreInstanceId, const Station& station,
                         const ModuleCatalog& catalog);

// Compose find -> solve -> collision-check into a ready-to-execute placement
// command, or nullptr if there is no free compatible target in `radius` or the
// resulting placement would collide. The two nullptr reasons (no in-range target
// vs. would-collide) are deliberately merged. The chosen target is excluded from
// the collision test (a direct joint is collision-free by construction, spec §6).
[[nodiscard]] std::unique_ptr<Command> makeSnapPlacement(const ModuleDef& newDef,
                                                         Vec3 cursorWorldPos,
                                                         const Station& station,
                                                         const ModuleCatalog& catalog,
                                                         double radius);

}  // namespace x4sb

#pragma once
// Snap engine — the core geometric primitive shared by manual and auto modes
// (spec §6). Pure geometry; no rendering.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

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
                                               double radius, InstanceId ignoreInstanceId = 0);

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

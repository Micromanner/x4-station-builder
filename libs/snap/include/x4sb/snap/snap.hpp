#pragma once
// Snap engine — the core geometric primitive shared by manual and auto modes
// (spec §6). Pure geometry; no rendering.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace x4sb {

// Rigid-body transform placing newDef's `newPointId` exactly onto the target
// instance's `targetPointId`, rotated 180° about the connector axis so the two
// connectors face each other. Consequence: a directly-connected joint is
// collision-free by construction.
Transform computeSnapTransform(const Station& station, const ModuleCatalog& catalog,
                               InstanceId targetInstanceId, const std::string& targetPointId,
                               const ModuleDef& newDef, const std::string& newPointId);

struct SnapCandidate {
  InstanceId instanceId{0};
  std::string targetPointId;
  std::string newPointId;
};

// Nearest free, compatible connection point within `radius`, or nullopt.
std::optional<SnapCandidate> findSnapCandidate(const ModuleDef& newDef, Vec3 cursorWorldPos,
                                               const Station& station, const ModuleCatalog& catalog,
                                               double radius);

// AABB overlap test of a candidate placement against all OTHER placed modules
// (the joint itself cannot overlap). `ignoreInstanceId` is skipped.
bool collidesWithStation(const ModuleDef& def, const Transform& worldTransform,
                         InstanceId ignoreInstanceId, const Station& station,
                         const ModuleCatalog& catalog);

}  // namespace x4sb

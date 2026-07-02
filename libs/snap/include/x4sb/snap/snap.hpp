#pragma once
// Snap engine — the core geometric primitive shared by manual and auto modes
// (spec §6). Pure geometry; no rendering.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"
#include "x4sb/snap/connector_grid.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace x4sb {

// Rigid-body transform placing newDef's `newPointId` exactly onto the target
// instance's `targetPointId`, rotated 180° about the connector axis so the two
// connectors face each other. Consequence: a directly-connected joint is
// collision-free by construction. Returns an identity Transform if the target
// instance or either connection point cannot be resolved.
// `clockingRad` rolls the new module about the shared connector axis (its local +Z
// normal); 0 reproduces the single fixed mate. The roll preserves point-coincidence
// and opposing normals, so any clocking is a valid joint — it selects which way the
// new module faces around the axis (e.g. the 90° turn that closes a flat ring).
Transform computeSnapTransform(const Station& station, const ModuleCatalog& catalog,
                               InstanceId targetInstanceId, const std::string& targetPointId,
                               const ModuleDef& newDef, const std::string& newPointId,
                               double clockingRad = 0.0);

// The new module's connector + clocking whose resulting orientation best matches a
// desired world rotation, with the solved mate transform. Used to honor the user's
// rotation when snapping (and reusable by auto-layout ring-tiling).
struct MateChoice {
  std::string newPointId;
  double clockingRad{0.0};
  Transform xf;
};

// Among newDef's connectors compatible with the target instance's `targetPointId`, the
// (connector, clocking) whose mate orientation is nearest `desiredWorldRot`. Clocking =
// twistAngle of (desiredWorldRot * conj(base)) about the target connector's world normal.
// nullopt if the target/point cannot be resolved or no compatible connector exists; a
// tie resolves to the lower connector index.
[[nodiscard]] std::optional<MateChoice> chooseMate(
    const Station& station, const ModuleCatalog& catalog, InstanceId targetInstanceId,
    const std::string& targetPointId, const ModuleDef& newDef, const Quat& desiredWorldRot);

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
std::optional<SnapCandidate> findSnapCandidate(
    const ModuleDef& newDef, Vec3 cursorWorldPos, const Station& station,
    const ModuleCatalog& catalog, double radius, InstanceId ignoreInstanceId = 0,
    std::optional<Transform> newDefTransform = std::nullopt);

// Grid-accelerated equivalent of findSnapCandidate: identical result, but uses a
// prebuilt ConnectorGrid to consider only connectors near the query point instead
// of scanning every placed module (known-issues 1.2). `grid` must have been built
// from the same `station`/`catalog`. `queryPoint` is the cursor world position
// (cursor mode) or the dragged pose's position (snap-on-move; pass newDefTransform).
[[nodiscard]] std::optional<SnapCandidate> findSnapCandidate(
    const ModuleDef& newDef, Vec3 queryPoint, const Station& station, const ModuleCatalog& catalog,
    const ConnectorGrid& grid, double radius, InstanceId ignoreInstanceId = 0,
    std::optional<Transform> newDefTransform = std::nullopt);

// AABB overlap test of a candidate placement against all OTHER placed modules
// (the joint itself cannot overlap). `ignoreInstanceId` is skipped.
bool collidesWithStation(const ModuleDef& def, const Transform& worldTransform,
                         InstanceId ignoreInstanceId, const Station& station,
                         const ModuleCatalog& catalog);

// As above, but skips up to two instances — the moved module and its snap partner
// during a drag-commit. Either id may be 0 (no instance has id 0).
[[nodiscard]] bool collidesWithStation(const ModuleDef& def, const Transform& worldTransform,
                                       InstanceId ignoreA, InstanceId ignoreB,
                                       const Station& station, const ModuleCatalog& catalog);

// Clearance-volume overlap test (known-issues §3.1): true if the candidate's
// dock/cradle clearance volumes intersect any other placed module's body AABB,
// OR if any other module's clearance volume intersects the candidate's body AABB.
// `ignoreA`/`ignoreB` are skipped (the dragged module and its snap partner); either
// may be 0.
[[nodiscard]] bool collidesClearance(const ModuleDef& def, const Transform& worldTransform,
                                     InstanceId ignoreA, InstanceId ignoreB, const Station& station,
                                     const ModuleCatalog& catalog);

// One placed dock's corridor that a candidate body intrudes: the dock's instance id
// and the index of the offending volume in that dock's `clearanceVolumes`. Lets the
// editor reveal exactly which corridor a placement/drag is blocking.
struct ClearanceHit {
  InstanceId blocker{0};
  std::size_t volumeIndex{0};
};

// The "their corridor vs my body" half of collidesClearance, but enumerated: every
// existing dock corridor that a body (`def` at `worldTransform`) intrudes. Empty if
// none. `ignoreA`/`ignoreB` are skipped (the moved module + its snap partner); either
// may be 0. (A candidate's OWN corridors are drawn separately by the editor, so they
// are not reported here.)
[[nodiscard]] std::vector<ClearanceHit> violatedClearance(const ModuleDef& def,
                                                          const Transform& worldTransform,
                                                          InstanceId ignoreA, InstanceId ignoreB,
                                                          const Station& station,
                                                          const ModuleCatalog& catalog);

// Preview pose for rolling a placed module with the rotation gizmo. If `hasSingleJoint`,
// the module pivots about its connector point for ANY axis (the joint point stays fixed,
// the body hinges around where it's attached) and stays mated. Otherwise it free-rotates
// about its start position (in place, no teleport) and reports the mate broken — used for a
// module with zero or 2+ joints (no single pivot). The caller decides how far to rotate
// (e.g. clamping by collision); this only solves the pose.
struct RollResult {
  Transform preview;
  bool mated{false};
};

[[nodiscard]] RollResult rollAboutConnector(const Transform& startTransform, Vec3 gizmoAxisWorld,
                                            double radians, Vec3 connectorWorldPos,
                                            bool hasSingleJoint);

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

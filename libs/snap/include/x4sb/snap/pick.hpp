#pragma once
// Render-free picking geometry (spec §7): the editor feeds raylib's mouse ray to
// these; no picking logic lives in the render layer.
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/document/station.hpp"

#include <optional>

namespace x4sb {

// Nearest non-negative intersection distance of the ray (origin + t*dir, t >= 0)
// with an axis-aligned box, or nullopt on a miss. `dir` need not be normalized;
// `t` is measured in units of `dir` length. Slab method.
[[nodiscard]] std::optional<double> rayIntersectsAabb(Vec3 origin, Vec3 dir, const AABB& box);

// The placed module whose world-AABB the ray hits nearest, or nullopt.
[[nodiscard]] std::optional<InstanceId> pickModule(const Station& station,
                                                   const ModuleCatalog& catalog, Vec3 rayOrigin,
                                                   Vec3 rayDir);

}  // namespace x4sb

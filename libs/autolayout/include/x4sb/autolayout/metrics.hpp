#pragma once
// Shared layout metrics (spec §8): the geometry the Compact/Pretty objectives reuse —
// the body-center centroid, radius of gyration (Compact's primary "pack into a blob"
// term), and merged world bounds. Pure queries; no search, no rendering.
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/document/station.hpp"

namespace x4sb {

// Mean of module body centers (apply(worldTransform, aabb-center)). {0,0,0} if empty.
[[nodiscard]] Vec3 centroid(const Station& station, const ModuleCatalog& catalog);

// Mean squared distance of body centers from the centroid (radius of gyration squared).
// 0 for an empty or single-module station. Minimizing it packs modules toward the core.
[[nodiscard]] double radiusOfGyrationSq(const Station& station, const ModuleCatalog& catalog);

// Merged world AABB over every body. Default AABB{} (a zero box at the origin) if empty.
[[nodiscard]] AABB bounds(const Station& station, const ModuleCatalog& catalog);

}  // namespace x4sb

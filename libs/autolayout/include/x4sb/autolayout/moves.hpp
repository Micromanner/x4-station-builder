#pragma once
// Structural moves for the SA search (spec §10). Compact uses only rehostMove (re-host
// is the Rg^2 lever — it relocates a module to a denser connector). Reorient/mirror are
// Pretty's appearance levers and land in Phase 4. Pure: each returns a candidate Station
// or nullopt; the anneal driver does the legality (collision) check.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <optional>
#include <random>

namespace x4sb {

// Re-host one randomly-chosen movable LEAF (a module with instanceId >= minMovableId,
// a single lower-id parent link, and no higher-id child links) onto a DIFFERENT free
// compatible connector on some lower-id module. Leaf-only avoids dislocating a subtree;
// lower-id-target-only preserves additivity and the ascending-id emit invariant. Returns
// the candidate (links + transform updated, reciprocity preserved) or nullopt if no such
// re-host exists. Legality (collision/plot) is the caller's (anneal) concern.
[[nodiscard]] std::optional<Station> rehostMove(const Station& station,
                                                const ModuleCatalog& catalog, std::mt19937& rng,
                                                InstanceId minMovableId);

}  // namespace x4sb

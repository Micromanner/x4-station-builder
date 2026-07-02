#pragma once
// Deterministic simulated-annealing driver (spec §11): from a legal seed, propose a
// move, reject if illegal, accept by exp(-delta/T) on a geometric cooling schedule,
// and return the best legal state seen. All randomness flows through one mt19937 seeded
// from opts.seed, so same seed + same inputs => byte-identical result. The objective and
// move set are supplied by the caller (Phases 3-4); this driver is objective-agnostic.
#include "x4sb/autolayout/search.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <vector>

namespace x4sb {

struct AutoLayoutOptions;  // defined in autolayout.hpp

[[nodiscard]] Station anneal(Station seed, const ModuleCatalog& catalog, const Objective& objective,
                             const std::vector<Move>& moves, const AutoLayoutOptions& opts);

}  // namespace x4sb

#pragma once
// Placement extraction (spec §5 finalize): serialize an optimized working station into
// ordered LayoutPlacements the editor replays as one CompositeCommand. Added modules
// (absent from `existing`) are emitted in ascending instanceId order; each links onto its
// lower-id parent (snapped) or is a floating root. Ascending-id order makes the replay
// re-assign matching ids so each snapped target resolves (commands.cpp; the parent is
// always placed first).
#include "x4sb/autolayout/autolayout.hpp"  // LayoutPlacement
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <vector>

namespace x4sb {

[[nodiscard]] std::vector<LayoutPlacement> extractPlacements(const Station& work,
                                                             const Station& existing,
                                                             const ModuleCatalog& catalog);

}  // namespace x4sb

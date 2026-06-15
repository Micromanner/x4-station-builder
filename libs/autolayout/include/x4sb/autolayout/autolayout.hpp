#pragma once
// Auto-layout engine (spec §7): quick-and-dirty layout from a quantity list,
// driving the snap engine in a loop. Explicitly not optimized for density or
// appearance — manual mode refines the result.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <string>
#include <unordered_map>

namespace x4sb {

using QuantityList = std::unordered_map<std::string, int>;  // macro id -> count

// Build a Station from a quantity list. Places module #1 at origin, snaps each
// subsequent module onto a free compatible connector (collision-checked), and
// places docks last on the outer hull, outward-facing.
Station autoLayout(const QuantityList& quantities, const ModuleCatalog& catalog);

}  // namespace x4sb

#include "x4sb/autolayout/autolayout.hpp"

#include "x4sb/snap/snap.hpp"

namespace x4sb {

// TODO(autolayout): implement the spec §7 loop —
//   1. group the quantity list by category,
//   2. place #1 at origin; for each subsequent module pick a free compatible
//      connector (prefer same-group neighbour), computeSnapTransform, accept if
//      collision-free, else try the next connector or start a parallel branch,
//   3. place docks last on the outer hull, outward-facing.
// Stubbed for now so the unit and its wiring exist; returns an empty Station.
Station autoLayout(const QuantityList& quantities, const ModuleCatalog& catalog) {
  (void)quantities;
  (void)catalog;
  return Station{};
}

}  // namespace x4sb

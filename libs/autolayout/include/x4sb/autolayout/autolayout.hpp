#pragma once
// Auto-layout engine (spec §7): quick-and-dirty layout from a quantity list, driving
// the snap engine in a loop. Additive by construction (parent design "Next up"): only
// adds the cart onto existing structure, never moves placed modules; greenfield = the
// empty-station case. Explicitly NOT optimized for density or appearance — manual mode
// refines the result.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace x4sb {

using QuantityList = std::unordered_map<std::string, int>;  // macro id -> count

struct AutoLayoutOptions {
  double plotHalfExtent = 10000.0;  // ±10 km plot; passed in so the engine stays decoupled
  double floatingMargin = 200.0;    // gap inserted between floating clusters
};

struct AutoLayoutReport {
  int requested = 0;  // total modules asked for (sum of cart counts)
  int snapped = 0;    // attached to a connector
  int floating = 0;   // placed as an unconnected root (greenfield seed + overflow)
  int skipped = 0;    // unknown / non-buildable / plot-saturated; never placed
  std::vector<std::string> skippedDefs;
};

// One module the layout added, recording exactly how, so the editor can rebuild the
// batch as undoable PlaceModuleCommands without re-deriving links. snapped=false is a
// floating root (the greenfield seed or an overflow drop).
struct LayoutPlacement {
  std::string defId;
  Transform worldTransform{};
  bool snapped = false;
  InstanceId targetInstanceId = 0;        // valid iff snapped
  std::string newPointId;                 // valid iff snapped
  std::string targetPointId;              // valid iff snapped
  InstanceId instanceId = 0;              // id assigned during the simulation
};

struct AutoLayoutResult {
  Station station;                          // existing + additions, fully linked (simulated)
  std::vector<LayoutPlacement> placements;  // additions, in placement order
  AutoLayoutReport report;
};

// §4.1 — expand the cart into the deterministic placement order: category priority
// (connector→production→storage→habitat→defense→other→dock), then largest AABB first,
// then def id. Unknown / non-buildable ids go to `skipped` (deduped, sorted), not placed.
struct OrderedCart {
  std::vector<std::string> placement;  // expanded def ids, ordered
  std::vector<std::string> skipped;    // unknown / non-buildable ids (deduped, sorted)
  int requested = 0;                   // sum of all cart counts
};
[[nodiscard]] OrderedCart orderedPlacement(const QuantityList& cart, const ModuleCatalog& catalog);

// Build the cart additively onto `existing` (never mutated): snap-primary, floating
// fallback, docks last. Returns the simulated station + per-module placements + report.
[[nodiscard]] AutoLayoutResult autoLayout(const Station& existing, const QuantityList& cart,
                                          const ModuleCatalog& catalog,
                                          const AutoLayoutOptions& opts = {});

}  // namespace x4sb

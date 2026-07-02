#pragma once
// Auto-layout engine (spec §7/§9): build a compact, connected station from a quantity list —
// a beam-spaced skeleton of connector hubs with the cart's modules combed off it, refined by a
// short simulated-annealing pass. Additive by construction: only adds the cart onto existing
// structure, never moves placed modules; greenfield = the empty-station case. The result is a
// valid starting point the player refines manually, not finished art.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace x4sb {

using QuantityList = std::unordered_map<std::string, int>;  // macro id -> count

// Which seed builds the layout. Fishbone = the planned connector skeleton (default, unchanged);
// Organic = dense module-as-structure clustering (spec §9.1 organic-clustering). Separate modes
// so the two can be A/B'd; Organic never replaces Fishbone.
enum class LayoutMode { Fishbone, Organic };

struct AutoLayoutOptions {
  double plotHalfExtent = 10000.0;  // ±10 km plot; passed in so the engine stays decoupled
  double floatingMargin = 200.0;    // gap inserted between hubs / floating clusters

  // Fishbone seed: the connector skeleton (backbone + ribs) sits this fraction of the plot
  // up toward +Y, so production columns have the center-and-below to grow DOWN into and the
  // band above stays clear for defense (spec §9.1 production-line rework).
  double skeletonVerticalBias = 0.35;

  // Search params for the SA refinement. Fixed defaults keep runs deterministic & bounded —
  // same cart + same seed ⇒ identical station (spec §3).
  std::uint32_t seed = 0x5f3759df;
  int maxIters = 4000;
  double startTemp = 1.0;
  double endTemp = 1e-3;

  double wMaterial = 1.0;  // connector-material penalty in the objective (spec §8.1)
  bool shuffle = false;
  bool mixRibModules = true;
  std::string faction = "";
  LayoutMode mode = LayoutMode::Fishbone;  // default preserves the existing Fishbone behavior
};

struct AutoLayoutReport {
  int requested = 0;        // functional modules asked for (sum of cart counts)
  int snapped = 0;          // attached to a connector
  int floating = 0;         // placed as an unconnected root
  int skipped = 0;          // unknown / non-buildable / plot-saturated; never placed
  int connectorsAdded = 0;  // structural pieces the engine synthesized (spec §3)
  std::vector<std::string> skippedDefs;
};

// One module the layout added, recording exactly how, so the editor can rebuild the
// batch as undoable PlaceModuleCommands without re-deriving links. snapped=false is a
// floating root (the greenfield seed or an overflow drop).
struct LayoutPlacement {
  std::string defId;
  Transform worldTransform{};
  bool snapped = false;
  InstanceId targetInstanceId = 0;  // valid iff snapped
  std::string newPointId;           // valid iff snapped
  std::string targetPointId;        // valid iff snapped
  InstanceId instanceId = 0;        // id assigned during the simulation
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
[[nodiscard]] OrderedCart orderedPlacement(const QuantityList& cart, const ModuleCatalog& catalog,
                                           const AutoLayoutOptions& opts = {});

// Build the cart additively onto `existing` (never mutated): snap-primary, floating
// fallback, docks last. Returns the simulated station + per-module placements + report.
[[nodiscard]] AutoLayoutResult autoLayout(const Station& existing, const QuantityList& cart,
                                          const ModuleCatalog& catalog,
                                          const AutoLayoutOptions& opts = {});

}  // namespace x4sb

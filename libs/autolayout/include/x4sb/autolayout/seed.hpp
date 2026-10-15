#pragma once
// Compact hub-cluster seed (spec §9.1): synthesize connection hubs and attach the
// interior cart onto them (largest first), so functional modules share dense junctions
// instead of snaking into chains. Produces a LEGAL starting layout the SA optimizer then
// tightens. Deterministic (no rng). Degrades to direct snapping / floating if the catalog
// has no usable hub.
#include "x4sb/autolayout/autolayout.hpp"  // AutoLayoutOptions
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <string>
#include <vector>

namespace x4sb {

// Build onto `work` (which may already hold `existing` modules). `interiorOrdered` is the
// interior cart's def ids; the seed re-sorts them largest-first internally. Returns the
// count of synthesized Category::Connector modules (the report's connectorsAdded).
int compactSeed(Station& work, const std::vector<std::string>& interiorOrdered,
                const ModuleCatalog& catalog, const AutoLayoutOptions& opts);

// Fishbone seed (spec §9.1, production-line rework): a planned connector skeleton instead of a
// greedy snaking one. A horizontal backbone (+X) sits high at opts.skeletonVerticalBias of the
// plot; one branch per module TYPE hangs off it — a rib (+Z) whose hubs each drop a vertical
// sub-spine (−Y), with that type's modules combed off the sub-spine hubs' side ports and the
// bottom port left open for a dock. Same signature/contract as compactSeed (returns synthesized
// Category::Connector count, builds additively). Degrades to compactSeed when the catalog has no
// hub the cart can mate; per-type overflow falls back to greedy attach/float so nothing is dropped.
int fishboneSeed(Station& work, const std::vector<std::string>& interiorOrdered,
                 const ModuleCatalog& catalog, const AutoLayoutOptions& opts);

// Organic-clustering seed (spec §9.1 organic-clustering): treat functional modules AS the structure
// — order them cores-first (most ports), chain each onto the most-free open port, and synthesize a
// structural hub only when free ports run low, a chain gets too long, or a layer needs a new
// vertical floor. Same signature/contract as compactSeed (returns synthesized Category::Connector
// count, builds additively). Deterministic (no rng). Degrades to floating when the catalog has no
// usable hub.
int organicSeed(Station& work, const std::vector<std::string>& interiorOrdered,
                const ModuleCatalog& catalog, const AutoLayoutOptions& opts);

// One escape-ladder rung for `forDef`: synthesize the best-mating hub — or, where its body cannot
// fit, a long thin span — onto the best open seat (ports of `preferHost` first so a ladder extends
// outward, then centroid-nearest). Returns the injected instance id (0 = nothing seats anywhere)
// and bumps `connectorsAdded`. Lets autoLayout grow an arm for a peripheral placePeripheral cannot
// seat in a dense interior, instead of dropping it.
InstanceId injectStructureFor(Station& work, const ModuleCatalog& catalog, const ModuleDef& forDef,
                              const AutoLayoutOptions& opts, int& connectorsAdded,
                              InstanceId preferHost);

// Free-fall floating fallback: place `def` as a collision-checked floating root at the legal
// in-plot pose nearest the station (X4 needs no connectivity; a float's ports stay live). The
// old +X-march fallback dropped modules silently at the plot edge — the 'skipped' count of
// known-issues 4.3. Returns false only when no in-plot pose on the y=0 plane is legal.
bool floatInsidePlot(Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
                     const AutoLayoutOptions& opts);

}  // namespace x4sb

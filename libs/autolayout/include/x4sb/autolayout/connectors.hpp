#pragma once
// Connector-vocabulary selection (spec §7): pick faction-matched structural pieces
// (hubs = high-port crosses, spans = 2-port straights) the Compact/Pretty seeds use
// to synthesize connective skeleton. Deterministic: candidates are sorted by an
// explicit key before choosing (ModuleCatalog::all() is unordered).
#include "x4sb/data/catalog.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace x4sb {

// The faction token of an id: the second '_'-delimited token (e.g.
// "struct_arg_cross_01_macro" -> "arg"). Empty if the id has no second token.
// Heuristic — adequate for the "struct_<faction>_*" vocabulary (spec §7).
[[nodiscard]] std::string factionOf(const std::string& id);

[[nodiscard]] std::size_t portCount(const ModuleDef& def);

// Smallest Category::Connector def with >= minPorts ports. Prefers `faction`, then
// fewest ports (least wasted material), then id. nullptr if none qualifies.
[[nodiscard]] const ModuleDef* bestHub(const ModuleCatalog& catalog, const std::string& faction,
                                       std::size_t minPorts);

// Like bestHub, but only considers hubs that can ACTUALLY host the cart: a hub qualifies
// only if >= 1 of its ports is connector-compatible (untagged-or-equal) with >= 1 of
// `needTypes` (the distinct connector types of the functional modules to attach). Without
// this, faction selection picks traps like struct_gen_venturercross — its "snap venture"
// ports faction-match "gen" modules but mate nothing plain "snap", so every module floats
// and the seed breeds empty hubs (spec §7). An empty type in `needTypes` matches anything.
[[nodiscard]] const ModuleDef* bestHubFor(const ModuleCatalog& catalog, const std::string& faction,
                                          std::size_t minPorts,
                                          const std::vector<std::string>& needTypes);

// A 2-port Category::Connector straight span. Prefers `faction`, then id. nullptr if none.
[[nodiscard]] const ModuleDef* bestSpan(const ModuleCatalog& catalog, const std::string& faction);

// Port-to-port distance of a 2-port connector (its span length). 0 if not a 2-port connector.
[[nodiscard]] double spanLength(const ModuleDef& def);

// The shortest 2-port span (beam) whose length is >= `minLength` and whose ports are
// compatible with `needTypes`, so the backbone can space hubs by the cart's module footprint
// (spec §9.1). Falls back to the LONGEST compatible span when none reaches `minLength` (best
// effort). Prefers `faction`, then shorter, then id. nullptr if no compatible span exists.
[[nodiscard]] const ModuleDef* bestSpanFor(const ModuleCatalog& catalog, const std::string& faction,
                                           const std::vector<std::string>& needTypes,
                                           double minLength);

}  // namespace x4sb

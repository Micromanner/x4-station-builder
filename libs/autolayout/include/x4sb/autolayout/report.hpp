#pragma once
// Human-readable topology report for an auto-layout result (diagnostic, spec §7/§9).
// Screenshots cannot reveal whether a layout is one connected station or three floating
// blobs, whether functional modules chain directly (spaghetti) or share connector hubs,
// or how well hub ports are utilized. This renders those graph properties as deterministic
// plain text so a layout's STRUCTURE can be read and compared across modes. Pure logic; no
// rendering, no I/O.
#include "x4sb/autolayout/autolayout.hpp"
#include "x4sb/data/catalog.hpp"

#include <string>

namespace x4sb {

// A deterministic, scannable text report of a layout's connectivity graph: header counts,
// connected components (a valid X4 plan is ONE), functional<->functional links (should be
// ~0 — functional modules belong on hubs), degree distribution, longest functional chain,
// per-hub port utilization, world bounds, radius of gyration, and a compact per-module
// table. `label` is echoed in the header. Set `perModule` false to omit the table.
[[nodiscard]] std::string layoutReport(const AutoLayoutResult& result, const ModuleCatalog& catalog,
                                       const std::string& label, bool perModule = true);

}  // namespace x4sb

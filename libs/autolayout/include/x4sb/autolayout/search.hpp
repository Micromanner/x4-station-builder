#pragma once
// Search framework (spec §11): the callback interfaces the SA driver and the modes
// share, plus the hard legality filter every accepted state must pass. The concrete
// objectives (Compact/Pretty) and the structural move set arrive with their seeds in
// Phases 3-4; this header is the contract they implement.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <functional>
#include <optional>
#include <random>

namespace x4sb {

struct AutoLayoutOptions;  // defined in autolayout.hpp

// Scalar cost to MINIMIZE (spec §8). Lower is better; evaluated only on legal states.
// MUST return a finite value on every legal state — the SA driver's exp(-delta/T) is
// undefined for inf/nan and would corrupt acceptance decisions.
using Objective = std::function<double(const Station&, const ModuleCatalog&)>;

// Proposes a neighbour state from the current one, drawing all randomness from the
// engine's single mt19937 (so the whole run stays deterministic, spec §3). Returns
// nullopt to mean "no change this draw" — the driver skips it.
using Move =
    std::function<std::optional<Station>(const Station&, const ModuleCatalog&, std::mt19937&)>;

// The hard filter (spec §11): a state is legal iff every body is in-plot, no two
// NON-adjacent bodies overlap (directly-linked neighbours touch at the joint by
// construction and are skipped), and no body intrudes any dock clearance corridor.
[[nodiscard]] bool layoutLegal(const Station& station, const ModuleCatalog& catalog,
                               const AutoLayoutOptions& opts);

}  // namespace x4sb

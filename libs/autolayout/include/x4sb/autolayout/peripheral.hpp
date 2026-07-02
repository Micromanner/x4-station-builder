#pragma once
// Peripheral placement pass (spec §10): attach docks/turrets/solar to the OUTERMOST
// free compatible connector, oriented outward, with an unobstructed outward clear
// volume (docks: their real corridors; turrets/solar: a synthesized forward clear
// box — the "can't hit itself" rule). Render-free; deterministic.
#include "x4sb/autolayout/autolayout.hpp"
#include "x4sb/autolayout/roles.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <optional>

namespace x4sb {

// On success: adds `def` to `work` (with the reciprocal Link) and returns the
// placement. nullopt if no outer connector yields a legal, outward-clear mate (the
// caller may then float it). `work` must be non-empty (there must be a hull to mount on).
[[nodiscard]] std::optional<LayoutPlacement> placePeripheral(Station& work,
                                                             const ModuleCatalog& catalog,
                                                             const ModuleDef& def, Role role,
                                                             const AutoLayoutOptions& opts);

}  // namespace x4sb

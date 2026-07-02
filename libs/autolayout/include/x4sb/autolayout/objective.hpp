#pragma once
// Compact-mode objective (spec §8.1): cost = Rg^2 + wMaterial * connectorMaterial.
// Minimizing Rg^2 packs modules into a blob; the material term keeps the optimizer from
// adding connectors that don't earn their density.
#include "x4sb/autolayout/autolayout.hpp"
#include "x4sb/autolayout/search.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

namespace x4sb {

// count + summed AABB volume of every Category::Connector module (the synthesized
// structural overhead the objective penalizes).
[[nodiscard]] double connectorMaterial(const Station& station, const ModuleCatalog& catalog);

// Rg^2 + opts.wMaterial * connectorMaterial, as an Objective the SA driver minimizes.
[[nodiscard]] Objective compactObjective(const AutoLayoutOptions& opts);

}  // namespace x4sb

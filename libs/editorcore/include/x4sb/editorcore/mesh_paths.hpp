#pragma once
// The set of unique mesh asset paths a station needs to render — used by the editor
// to pre-upload every mesh up front (a loading screen) instead of streaming them in
// per frame, which for a 3D editor is the wrong model (known-issues 1.3). Pure data
// logic (ModuleDef::meshRefs), so it tests headlessly under the `core` preset.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <string>
#include <vector>

namespace x4sb {

// Distinct `gltfPath`s across every placed module's mesh refs, sorted (so the order
// is deterministic and repeated copies of a module collapse to one entry). Modules
// whose def is missing from the catalog contribute nothing.
[[nodiscard]] std::vector<std::string> meshPathsFor(const Station& station,
                                                    const ModuleCatalog& catalog);

}  // namespace x4sb

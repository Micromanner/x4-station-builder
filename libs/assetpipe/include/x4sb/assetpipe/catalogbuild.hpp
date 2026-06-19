#pragma once
// Build the full module catalog (spec section 2/4): resolve every buildable
// station module (libraries/wares.xml -> index/macros.xml -> macro XML ->
// index/components.xml -> component XML) into ModuleDefs. All file access goes
// through `extract`, so this is testable with an in-memory fixture map and reused
// by the editor's first-run wizard.
#include "x4sb/data/types.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace x4sb {

// Fetch a logical archive path's bytes (e.g. bind to Archive::extract).
using ExtractFn = std::function<std::optional<std::string>(const std::string&)>;

struct CatalogBuildResult {
  std::vector<ModuleDef> modules;
  std::vector<std::string> skipped;  // "wareId: reason" per dropped module
  std::size_t namesResolved{0};      // modules given a localized display name
  std::size_t namesUnresolved{0};    // modules left with empty name (UI falls back to id)
};

// Resolve every buildable station module across the given source overlays
// (sourcePrefixes: "" for the base game, "extensions/<dlc>/" per DLC) into
// ModuleDefs. Reads each source's libraries/wares.xml + index/{macros,components}.xml,
// accumulates wares, and merges the indexes (DLC index values are self-qualified).
// All file access goes through `extract`. Resilient: a module that fails to
// resolve is recorded in `skipped` and does not abort the build.
[[nodiscard]] CatalogBuildResult buildModuleCatalog(const ExtractFn& extract,
                                                    const std::vector<std::string>& sourcePrefixes);

// The cache-relative glTF path the catalog records for a module's part mesh:
// "meshes/<moduleId>__<partName>.gltf". The single source of truth shared by the
// catalog builder and the batch mesh converter so their paths cannot drift.
[[nodiscard]] std::string meshGltfPath(const std::string& moduleId, const std::string& partName);

}  // namespace x4sb

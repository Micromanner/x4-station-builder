#pragma once
// Launch-dir-independent location of the asset cache. Shared by the interactive
// entry point and the --snaptest harness so both resolve catalog.json the same way.
#include <optional>
#include <string>

namespace x4sb::editor {

// Locate asset-cache/catalog.json regardless of the launch directory (CWD or a
// double-clicked exe). Searches from both the working directory and the
// executable's directory, walking up a few levels so a build/<preset>/bin/ exe
// still finds the repo-root cache. Returns the first existing path, or nullopt.
[[nodiscard]] std::optional<std::string> findCatalogJson();

}  // namespace x4sb::editor

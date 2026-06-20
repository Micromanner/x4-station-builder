#pragma once
// Launch-dir-independent asset location. Shared by the interactive entry point
// and the --snaptest harness so both resolve paths the same way.
#include <optional>
#include <string>

namespace x4sb::editor {

// Locate an asset by repo-relative path regardless of launch dir (same walk-up
// search as findCatalogJson: from CWD and the exe dir, up a few levels). Returns
// the first existing path, or nullopt.
[[nodiscard]] std::optional<std::string> findAsset(const std::string& rel);

// Convenience wrapper: findAsset("asset-cache/catalog.json").
[[nodiscard]] std::optional<std::string> findCatalogJson();

}  // namespace x4sb::editor

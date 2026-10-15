#pragma once
// Launch-dir-independent lookup of repo-bundled assets (UI fonts).
#include <optional>
#include <string>

namespace x4sb::editor {

// Locate an asset by repo-relative path regardless of launch dir (walks up from
// CWD and the exe dir a few levels). Returns the first existing path, or nullopt.
[[nodiscard]] std::optional<std::string> findAsset(const std::string& rel);

}  // namespace x4sb::editor

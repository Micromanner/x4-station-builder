#pragma once

#include "x4sb/archive/archive.hpp"
#include "x4sb/assetpipe/material_library.hpp"
#include "x4sb/data/catalog.hpp"

#include <memory>
#include <optional>
#include <string>

namespace x4sb::editor {

struct NativeAssets {
  std::shared_ptr<const Archive> archive;
  MaterialLibrary materials;
  ModuleCatalog catalog;
  double metadataSeconds{};
  bool metadataCacheHit{};
};

[[nodiscard]] std::optional<NativeAssets> loadNativeAssets(const std::string& gameDirectory);
// Harness modes: `gameDirectory` (from --x4), else the saved install. Explains
// failures on stderr.
[[nodiscard]] std::optional<NativeAssets> loadHarnessAssets(const std::string& gameDirectory);
// First-run/recovery UI; returns nullopt when the window is closed.
[[nodiscard]] std::optional<std::string> chooseGameDirectory(const std::string& initial,
                                                             bool previousLoadFailed);

}  // namespace x4sb::editor

#include "app_paths.hpp"

#include "raylib.h"

#include <array>

namespace x4sb::editor {

std::optional<std::string> findAsset(const std::string& rel) {
  const std::array<std::string, 2> bases{std::string{}, std::string{GetApplicationDirectory()}};
  const std::array<std::string, 5> ups{"", "../", "../../", "../../../", "../../../../"};
  for (const auto& base : bases) {
    for (const auto& up : ups) {
      const std::string path = base + up + rel;
      if (FileExists(path.c_str())) return path;
    }
  }
  return std::nullopt;
}

std::optional<std::string> findCatalogJson() { return findAsset("asset-cache/catalog.json"); }

}  // namespace x4sb::editor

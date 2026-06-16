#include "mesh_cache.hpp"

#include <system_error>
#include <utility>

namespace x4sb::editor {

MeshCache::MeshCache(std::filesystem::path assetRoot) : root_(std::move(assetRoot)) {}

MeshCache::~MeshCache() {
  for (auto& [key, model] : cache_) {
    if (model.has_value()) UnloadModel(model.value());
  }
}

const ::Model* MeshCache::get(const std::string& gltfPath) {
  const auto it = cache_.find(gltfPath);
  if (it != cache_.end()) return it->second.has_value() ? &it->second.value() : nullptr;

  const std::filesystem::path full = root_ / gltfPath;
  std::error_code ec;
  if (!std::filesystem::exists(full, ec) || ec) {
    // One warning per missing path (the negative cache guarantees one lookup
    // each), so interactive validation can see which modules box-fell-back.
    // LoadModel failures are already TraceLog'd by raylib, so only this branch.
    TraceLog(LOG_WARNING, "MeshCache: missing %s", full.string().c_str());
    cache_.emplace(gltfPath, std::nullopt);
    return nullptr;
  }

  ::Model m = LoadModel(full.string().c_str());
  if (m.meshCount == 0) {  // load failed / empty model
    UnloadModel(m);
    cache_.emplace(gltfPath, std::nullopt);
    return nullptr;
  }

  const auto inserted = cache_.emplace(gltfPath, std::move(m)).first;
  return &inserted->second.value();
}

}  // namespace x4sb::editor

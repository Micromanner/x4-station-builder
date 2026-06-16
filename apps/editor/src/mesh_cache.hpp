#pragma once
// Load-once cache of glTF Models for the editor. Owns raylib Models (a C resource),
// so it is non-copyable/non-movable and unloads everything in its destructor.
// Lives only in apps/editor (the sole raylib-linking target); libs/* stay
// render-free. MUST be constructed AFTER InitWindow and destroyed BEFORE
// CloseWindow — LoadModel/UnloadModel both require a live GL context.
#include "raylib.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace x4sb::editor {

class MeshCache {
 public:
  explicit MeshCache(std::filesystem::path assetRoot);
  ~MeshCache();
  MeshCache(const MeshCache&) = delete;
  MeshCache& operator=(const MeshCache&) = delete;
  MeshCache(MeshCache&&) = delete;
  MeshCache& operator=(MeshCache&&) = delete;

  // Load-once, cached. `gltfPath` is asset-cache-relative (e.g.
  // "meshes/<id>__<part>.gltf"). Returns a pointer to the loaded Model, or
  // nullptr if the file is missing or failed to load (negative-cached so we
  // never retry or spam LoadModel). The returned pointer stays valid for the
  // lifetime of the cache (unordered_map node addresses are stable).
  [[nodiscard]] const ::Model* get(const std::string& gltfPath);

 private:
  std::filesystem::path root_;
  std::unordered_map<std::string, std::optional<::Model>> cache_;  // nullopt = known-missing
};

}  // namespace x4sb::editor

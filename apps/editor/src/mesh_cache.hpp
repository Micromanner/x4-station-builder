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
#include <unordered_set>
#include <vector>

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

  // True if `gltfPath` was rejected for exceeding raylib's 16-bit mesh-index
  // limit (>65535 vertices -> truncated indices -> garbage triangles). Valid only
  // after get(gltfPath) has been called (get() populates the verdict). Lets the
  // caller box the WHOLE module rather than draw a corrupt or partial hull.
  [[nodiscard]] bool isOversized(const std::string& gltfPath) const {
    return oversized_.count(gltfPath) != 0;
  }

  // Draw every instance of one cached part in a SINGLE GPU call (DrawMeshInstanced),
  // flat-shaded with `tint` — the big-station win, collapsing one draw call per
  // placed copy into one per unique part. `xforms` are model matrices in native X4
  // space; the global scene flip is carried by the instancing shader's mvp uniform,
  // exactly as the per-module path relies on the rlScalef stack. No-op when the part
  // is missing or the instancing shader failed to compile (caller boxes instead).
  void drawInstanced(const std::string& gltfPath, const std::vector<::Matrix>& xforms,
                     ::Color tint);

 private:
  std::filesystem::path root_;
  std::unordered_map<std::string, std::optional<::Model>> cache_;  // nullopt = known-missing
  std::unordered_set<std::string> oversized_;  // rejected: > 65535 verts (u16 index limit)
  // Flat-shading shader (derivative-based face normals + two-sided Lambert), shared
  // by every loaded model's material so solid modules read as 3D, not flat blobs.
  ::Shader flatShader_{};
  bool flatShaderOk_{false};
  // Instancing variant of the flat shader: identical lighting, but reads the model
  // matrix from a per-instance `instanceTransform` vertex attribute instead of a
  // uniform, so DrawMeshInstanced can batch every copy of a part into one call.
  ::Shader instancedShader_{};
  bool instancedShaderOk_{false};
};

}  // namespace x4sb::editor

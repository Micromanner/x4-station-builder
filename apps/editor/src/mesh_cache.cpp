#include "mesh_cache.hpp"

#include <system_error>
#include <utility>

namespace x4sb::editor {
namespace {

// Flat-shading shader. Face normals are reconstructed per-fragment from screen-
// space derivatives of the world position, so it needs NO vertex normals (the
// XMF->glTF converter doesn't always emit them) and shades each triangle flat.
// Lighting is two-sided (abs of N.L) so it's immune to the winding inversion the
// global (1,1,-1) handedness flip introduces and to which way the face points.
constexpr const char* kFlatVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragPosition;
void main() {
  fragTexCoord = vertexTexCoord;
  fragColor = vertexColor;
  fragPosition = vec3(matModel*vec4(vertexPosition, 1.0));
  gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";

constexpr const char* kFlatFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragPosition;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
out vec4 finalColor;
const vec3 kLightDir = normalize(vec3(0.45, 0.8, 0.4));
void main() {
  vec3 n = normalize(cross(dFdx(fragPosition), dFdy(fragPosition)));
  float diff = abs(dot(n, kLightDir));
  float shade = 0.32 + 0.68*diff;  // ambient floor + diffuse so no face is pure black
  vec4 texel = texture(texture0, fragTexCoord);
  finalColor = vec4(texel.rgb*colDiffuse.rgb*fragColor.rgb*shade, colDiffuse.a*fragColor.a);
}
)";

// raylib stores mesh indices as 16-bit, so a part with more than this many
// vertices loads with wrapped indices (renders as a spray of huge triangles).
constexpr int kU16IndexLimit = 65535;

}  // namespace

MeshCache::MeshCache(std::filesystem::path assetRoot) : root_(std::move(assetRoot)) {
  // GL context is live by contract (constructed after InitWindow), so compile here.
  flatShader_ = LoadShaderFromMemory(kFlatVs, kFlatFs);
  flatShaderOk_ = IsShaderValid(flatShader_);
}

MeshCache::~MeshCache() {
  for (auto& [key, model] : cache_) {
    if (model.has_value()) UnloadModel(model.value());
  }
  if (flatShaderOk_) UnloadShader(flatShader_);
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

  // Reject an oversized part (see kU16IndexLimit) so the caller boxes the module;
  // the real fix is de-indexing/splitting in the asset pipeline (see design
  // "Implementation status" / xmf-format memory).
  for (int i = 0; i < m.meshCount; ++i) {
    if (m.meshes[i].vertexCount > kU16IndexLimit) {
      TraceLog(LOG_WARNING, "MeshCache: %s exceeds u16 index limit (%d verts) - boxing module",
               gltfPath.c_str(), m.meshes[i].vertexCount);
      oversized_.insert(gltfPath);
      UnloadModel(m);
      cache_.emplace(gltfPath, std::nullopt);
      return nullptr;
    }
  }

  // Render every part flat-shaded instead of with raylib's unlit default shader,
  // which would draw solid modules as depth-less silhouettes.
  if (flatShaderOk_) {
    for (int i = 0; i < m.materialCount; ++i) m.materials[i].shader = flatShader_;
  }

  const auto inserted = cache_.emplace(gltfPath, std::move(m)).first;
  return &inserted->second.value();
}

}  // namespace x4sb::editor

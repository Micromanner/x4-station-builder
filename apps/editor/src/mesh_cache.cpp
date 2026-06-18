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

// Instancing variant of kFlatVs: the model matrix arrives as a per-instance vertex
// attribute (raylib binds the transforms array to `instanceTransform`) rather than a
// uniform, and `mvp` carries only camera*flip — so mvp*instanceTransform reproduces
// the per-module path's flip*world*local composition with no per-instance baking.
constexpr const char* kInstancedVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
in mat4 instanceTransform;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragPosition;
void main() {
  fragTexCoord = vertexTexCoord;
  fragColor = vertexColor;
  // Apply the global (1,1,-1) handedness flip here too: the per-module path derives
  // fragPosition from matModel, which carries that flip, and the flat shader builds
  // its face normal from fragPosition's screen-space derivatives. Matching the space
  // keeps instanced shading pixel-identical to the per-module path.
  vec4 wp = instanceTransform*vec4(vertexPosition, 1.0);
  fragPosition = vec3(wp.x, wp.y, -wp.z);
  gl_Position = mvp*instanceTransform*vec4(vertexPosition, 1.0);
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

  instancedShader_ = LoadShaderFromMemory(kInstancedVs, kFlatFs);
  instancedShaderOk_ = IsShaderValid(instancedShader_);
  if (instancedShaderOk_) {
    // DrawMeshInstanced binds the transforms array to the attribute whose location
    // lives in locs[SHADER_LOC_MATRIX_MODEL]; "mvp" is auto-detected by LoadShader.
    instancedShader_.locs[SHADER_LOC_MATRIX_MODEL] =
        GetShaderLocationAttrib(instancedShader_, "instanceTransform");
  }
}

MeshCache::~MeshCache() {
  for (auto& [key, model] : cache_) {
    if (model.has_value()) UnloadModel(model.value());
  }
  if (flatShaderOk_) UnloadShader(flatShader_);
  if (instancedShaderOk_) UnloadShader(instancedShader_);
}

const ::Model* MeshCache::tryLoad(const std::string& gltfPath, bool budgeted) {
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

  // get() (the per-frame path) defers when the frame's upload budget is spent:
  // return nullptr WITHOUT caching so the caller boxes the module for now and we
  // retry next frame — distinct from a genuine failure below, which negative-caches
  // and never retries. warm() (the loading-screen pre-upload) passes budgeted=false
  // to upload regardless. (known-issues 1.3)
  if (budgeted) {
    if (uploadsThisFrame_ >= kUploadsPerFrame) return nullptr;
    ++uploadsThisFrame_;
  }

  ::Model m = LoadModel(full.string().c_str());
  if (m.meshCount == 0) {  // load failed / empty model
    UnloadModel(m);
    cache_.emplace(gltfPath, std::nullopt);
    return nullptr;
  }

  // Reject an INDEXED oversized part (see kU16IndexLimit) so the caller boxes the
  // module: raylib's 16-bit indices would wrap into garbage. A de-indexed part (the
  // pipeline expands oversized meshes to a flat vertex stream with no index buffer)
  // draws via glDrawArrays and is immune, so only indexed meshes are gated here.
  for (int i = 0; i < m.meshCount; ++i) {
    if (m.meshes[i].indices != nullptr && m.meshes[i].vertexCount > kU16IndexLimit) {
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

const ::Model* MeshCache::get(const std::string& gltfPath) {
  return tryLoad(gltfPath, /*budgeted=*/true);
}

const ::Model* MeshCache::warm(const std::string& gltfPath) {
  return tryLoad(gltfPath, /*budgeted=*/false);
}

void MeshCache::drawInstanced(const std::string& gltfPath, const std::vector<::Matrix>& xforms,
                              ::Color tint) {
  if (!instancedShaderOk_ || xforms.empty()) return;
  const ::Model* model = get(gltfPath);
  if (model == nullptr) return;
  const int count = static_cast<int>(xforms.size());
  for (int i = 0; i < model->meshCount; ++i) {
    // Shallow-copy the part's own material (keeps its texture) but swap in the
    // instancing shader and the requested tint. maps is shared with the cached
    // model, so restore the colour afterwards to leave the model pristine for the
    // non-instanced path (selected module / ghost).
    const int mi = model->meshMaterial[i];
    ::Material mat = model->materials[mi];
    mat.shader = instancedShader_;
    const ::Color saved = mat.maps[MATERIAL_MAP_DIFFUSE].color;
    mat.maps[MATERIAL_MAP_DIFFUSE].color = tint;
    DrawMeshInstanced(model->meshes[i], mat, xforms.data(), count);
    mat.maps[MATERIAL_MAP_DIFFUSE].color = saved;
  }
}

}  // namespace x4sb::editor

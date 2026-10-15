#include "xmf_model.hpp"

#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace x4sb::editor {
namespace {

struct ModelCleanup {
  void operator()(::Model* model) const { UnloadModel(*model); }
};

template <typename T>
T* allocate(std::size_t count) {
  if (count > std::numeric_limits<unsigned int>::max() / sizeof(T)) return nullptr;
  // raylib's UnloadModel/UnloadMesh require its allocator, not C++ vector storage.
  auto* memory = static_cast<T*>(MemAlloc(static_cast<unsigned int>(count * sizeof(T))));
  if (memory != nullptr) std::uninitialized_value_construct_n(memory, count);
  return memory;
}

bool fitsFloat(double value) {
  return std::isfinite(value) &&
         std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
}

// The source's ranges (one default range when it has no table) once every stream,
// index and range is consistent; nullopt for a malformed mesh.
std::optional<std::vector<XmfSubmesh>> validatedRanges(const XmfMesh& source) {
  if (source.positions.empty() || source.indices.empty() || source.indices.size() % 3 != 0)
    return std::nullopt;
  const auto vertices = source.positions.size();
  if ((!source.normals.empty() && source.normals.size() != vertices) ||
      (!source.texcoords.empty() && source.texcoords.size() != vertices) ||
      (!source.colors.empty() && source.colors.size() != vertices) ||
      (!source.tangents.empty() &&
       (source.tangents.size() != vertices || source.bitangentSigns.size() != vertices)) ||
      (!source.texcoords1.empty() && source.texcoords1.size() != vertices) ||
      (!source.texcoords2.empty() && source.texcoords2.size() != vertices) ||
      (!source.texcoords3.empty() && source.texcoords3.size() != vertices) ||
      (!source.vertexData1.empty() && source.vertexData1.size() != vertices) ||
      (!source.vertexData2.empty() && source.vertexData2.size() != vertices))
    return std::nullopt;
  for (const auto index : source.indices)
    if (index >= vertices) return std::nullopt;
  for (const auto& p : source.positions)
    if (!fitsFloat(p.x) || !fitsFloat(p.y) || !fitsFloat(p.z)) return std::nullopt;
  for (const auto& n : source.normals)
    if (!fitsFloat(n.x) || !fitsFloat(n.y) || !fitsFloat(n.z)) return std::nullopt;
  for (const auto& uv : source.texcoords)
    if (!fitsFloat(uv.x) || !fitsFloat(uv.y)) return std::nullopt;
  for (const auto& t : source.tangents)
    if (!fitsFloat(t.x) || !fitsFloat(t.y) || !fitsFloat(t.z)) return std::nullopt;
  for (const auto& uv : source.texcoords1)
    if (!fitsFloat(uv.x) || !fitsFloat(uv.y)) return std::nullopt;
  for (const auto& uv : source.texcoords2)
    if (!fitsFloat(uv.x) || !fitsFloat(uv.y)) return std::nullopt;
  for (const auto& uv : source.texcoords3)
    if (!fitsFloat(uv.x) || !fitsFloat(uv.y)) return std::nullopt;

  auto ranges = source.submeshes;
  if (ranges.empty()) {
    if (source.indices.size() > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    ranges.push_back({0, static_cast<std::uint32_t>(source.indices.size()), {}});
  }
  if (ranges.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return std::nullopt;
  for (const auto& range : ranges) {
    if (range.indexCount == 0 || range.indexCount % 3 != 0 || range.firstIndex % 3 != 0 ||
        range.firstIndex > source.indices.size() ||
        range.indexCount > source.indices.size() - range.firstIndex)
      return std::nullopt;
  }
  return ranges;
}

}  // namespace

std::optional<std::vector<PreparedMesh>> prepareXmfModel(const XmfMesh& source, bool rawColors,
                                                         bool tangentFrame, bool uv1) {
  const auto validated = validatedRanges(source);
  if (!validated) return std::nullopt;
  const auto& ranges = *validated;
  const auto vertices = source.positions.size();
  const bool deindex = vertices > 65535;
  const bool uv23 = tangentFrame || !source.texcoords2.empty();
  const bool withUv1 = tangentFrame || uv1;
  std::vector<PreparedMesh> meshes(ranges.size());
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    const auto& range = ranges[i];
    auto& mesh = meshes[i];
    const std::size_t count = deindex ? range.indexCount : vertices;
    // raylib passes attribute byte sizes to OpenGL through signed ints.
    if (count > static_cast<std::size_t>(std::numeric_limits<int>::max()) / (3 * sizeof(float)))
      return std::nullopt;
    mesh.vertices.resize(count * 3);
    mesh.texcoords.resize(count * 2);
    mesh.colors.resize(count * 4);
    if (!source.normals.empty()) mesh.normals.resize(count * 3);
    if (tangentFrame) mesh.tangents.resize(count * 4);
    if (withUv1) mesh.texcoords2.resize(count * 2);
    if (uv23) mesh.texcoords3.resize(count * 4);
    for (std::size_t v = 0; v < count; ++v) {
      const auto index = deindex ? source.indices[range.firstIndex + v] : v;
      const auto& p = source.positions[index];
      mesh.vertices[v * 3] = static_cast<float>(p.x);
      mesh.vertices[v * 3 + 1] = static_cast<float>(p.y);
      mesh.vertices[v * 3 + 2] = -static_cast<float>(p.z);
      if (!mesh.normals.empty()) {
        const auto& n = source.normals[index];
        mesh.normals[v * 3] = static_cast<float>(n.x);
        mesh.normals[v * 3 + 1] = static_cast<float>(n.y);
        mesh.normals[v * 3 + 2] = -static_cast<float>(n.z);
      }
      if (!source.texcoords.empty()) {
        mesh.texcoords[v * 2] = static_cast<float>(source.texcoords[index].x);
        mesh.texcoords[v * 2 + 1] = static_cast<float>(source.texcoords[index].y);
      }
      if (tangentFrame && !source.tangents.empty()) {
        const auto& t = source.tangents[index];
        mesh.tangents[v * 4] = static_cast<float>(t.x);
        mesh.tangents[v * 4 + 1] = static_cast<float>(t.y);
        mesh.tangents[v * 4 + 2] = -static_cast<float>(t.z);
        // The Z reflection flips handedness: cross(Mn, Mt) = -M cross(n, t), so
        // the display-space bitangent is cross(n, t) * -sign.
        mesh.tangents[v * 4 + 3] = -source.bitangentSigns[index];
      }
      if (withUv1 && !source.texcoords1.empty()) {
        mesh.texcoords2[v * 2] = static_cast<float>(source.texcoords1[index].x);
        mesh.texcoords2[v * 2 + 1] = static_cast<float>(source.texcoords1[index].y);
      }
      if (!source.texcoords2.empty()) {
        mesh.texcoords3[v * 4] = static_cast<float>(source.texcoords2[index].x);
        mesh.texcoords3[v * 4 + 1] = static_cast<float>(source.texcoords2[index].y);
      }
      if (uv23 && !source.texcoords3.empty()) {
        mesh.texcoords3[v * 4 + 2] = static_cast<float>(source.texcoords3[index].x);
        mesh.texcoords3[v * 4 + 3] = static_cast<float>(source.texcoords3[index].y);
      }
      for (std::size_t channel = 0; channel < 3; ++channel)
        mesh.colors[v * 4 + channel] =
            source.colors.empty() ? (rawColors ? 255 : 128) : source.colors[index][channel];
      mesh.colors[v * 4 + 3] = source.colors.empty() ? 255 : source.colors[index][3];
    }
    if (!deindex) {
      mesh.indices.resize(range.indexCount);
      for (std::size_t index = 0; index < range.indexCount; ++index)
        mesh.indices[index] = static_cast<unsigned short>(source.indices[range.firstIndex + index]);
    }
  }
  return meshes;
}

std::optional<::Model> uploadXmfModel(const std::vector<PreparedMesh>& meshes) {
  if (meshes.empty() || meshes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return std::nullopt;
  ::Model model{};
  std::unique_ptr<::Model, ModelCleanup> cleanup(&model);
  model.transform = MatrixIdentity();
  model.meshes = allocate<::Mesh>(meshes.size());
  model.materials = allocate<::Material>(meshes.size());
  model.meshMaterial = allocate<int>(meshes.size());
  if (model.meshes == nullptr || model.materials == nullptr || model.meshMaterial == nullptr)
    return std::nullopt;
  model.meshCount = static_cast<int>(meshes.size());
  model.materialCount = model.meshCount;

  // raylib's UnloadModel frees these with its allocator, so copy out of the vectors.
  const auto copy = [](const auto& from) {
    using T = typename std::decay_t<decltype(from)>::value_type;
    T* to = from.empty() ? nullptr : allocate<T>(from.size());
    if (to != nullptr) std::copy(from.begin(), from.end(), to);
    return to;
  };
  for (std::size_t i = 0; i < meshes.size(); ++i) {
    const auto& source = meshes[i];
    auto& mesh = model.meshes[i];
    const std::size_t count = source.vertices.size() / 3;
    const std::size_t indexCount = source.indices.empty() ? count : source.indices.size();
    if (count == 0 || source.texcoords.size() != count * 2 || source.colors.size() != count * 4 ||
        (!source.normals.empty() && source.normals.size() != count * 3) ||
        (!source.tangents.empty() && source.tangents.size() != count * 4) ||
        (!source.texcoords2.empty() && source.texcoords2.size() != count * 2) ||
        (!source.texcoords3.empty() && source.texcoords3.size() != count * 4))
      return std::nullopt;
    mesh.vertexCount = static_cast<int>(count);
    mesh.triangleCount = static_cast<int>(indexCount / 3);
    mesh.vertices = copy(source.vertices);
    mesh.texcoords = copy(source.texcoords);
    mesh.colors = copy(source.colors);
    mesh.normals = copy(source.normals);
    mesh.tangents = copy(source.tangents);
    mesh.texcoords2 = copy(source.texcoords2);
    mesh.boneWeights = copy(source.texcoords3);
    mesh.indices = copy(source.indices);
    if (mesh.vertices == nullptr || mesh.texcoords == nullptr || mesh.colors == nullptr ||
        (!source.normals.empty() && mesh.normals == nullptr) ||
        (!source.tangents.empty() && mesh.tangents == nullptr) ||
        (!source.texcoords2.empty() && mesh.texcoords2 == nullptr) ||
        (!source.texcoords3.empty() && mesh.boneWeights == nullptr) ||
        (!source.indices.empty() && mesh.indices == nullptr))
      return std::nullopt;
    UploadMesh(&mesh, false);
    if (mesh.vaoId == 0 || mesh.vboId == nullptr || mesh.vboId[0] == 0) return std::nullopt;
    model.materials[i] = LoadMaterialDefault();
    if (model.materials[i].maps == nullptr) return std::nullopt;
    model.meshMaterial[i] = static_cast<int>(i);
  }
  (void)cleanup.release();
  return model;
}

std::uint32_t packSnorm10(float x, float y, float z) {
  const auto field = [](float v) {
    const auto q = static_cast<std::int32_t>(std::lround(std::clamp(v, -1.0F, 1.0F) * 511.0F));
    return static_cast<std::uint32_t>(q) & 0x3FFU;
  };
  return field(x) | (field(y) << 10U) | (field(z) << 20U);
}

std::optional<PreparedPart> prepareStationPart(const XmfMesh& source,
                                               const std::vector<bool>& extendedVertexData) {
  const auto ranges = validatedRanges(source);
  if (!ranges) return std::nullopt;
  const std::size_t vertices = source.positions.size();
  // Two blocks of a part this large would overflow the u32 vertex offsets.
  if (vertices > std::numeric_limits<std::uint32_t>::max() / 2) return std::nullopt;
  const auto isExtended = [&](std::size_t i) {
    return i < extendedVertexData.size() && extendedVertexData[i];
  };
  bool anyPlain = false;
  bool anyExtended = false;
  for (std::size_t i = 0; i < ranges->size(); ++i) (isExtended(i) ? anyExtended : anyPlain) = true;

  PreparedPart part;
  part.hasNormals = !source.normals.empty();
  part.vertices.reserve(vertices * (anyPlain && anyExtended ? 2 : 1));
  const auto appendBlock = [&](bool extended) {
    const auto& data = extended ? source.vertexData2 : source.vertexData1;
    for (std::size_t index = 0; index < vertices; ++index) {
      ArenaVertex& v = part.vertices.emplace_back();
      const auto& p = source.positions[index];
      v.position = {static_cast<float>(p.x), static_cast<float>(p.y), -static_cast<float>(p.z)};
      if (part.hasNormals) {
        const auto& n = source.normals[index];
        v.normal =
            packSnorm10(static_cast<float>(n.x), static_cast<float>(n.y), -static_cast<float>(n.z));
      }
      if (!source.texcoords.empty())
        v.uv0 = {static_cast<float>(source.texcoords[index].x),
                 static_cast<float>(source.texcoords[index].y)};
      if (!source.texcoords1.empty())
        v.uv1 = {static_cast<float>(source.texcoords1[index].x),
                 static_cast<float>(source.texcoords1[index].y)};
      if (!source.texcoords2.empty()) {
        v.uv23[0] = static_cast<float>(source.texcoords2[index].x);
        v.uv23[1] = static_cast<float>(source.texcoords2[index].y);
      }
      if (!source.texcoords3.empty()) {
        v.uv23[2] = static_cast<float>(source.texcoords3[index].x);
        v.uv23[3] = static_cast<float>(source.texcoords3[index].y);
      }
      for (std::size_t channel = 0; channel < 3; ++channel)
        v.color[channel] = source.colors.empty() ? 128 : source.colors[index][channel];
      // COLOR0 alpha is never opacity: it carries the dirt amount, X4's own (common.vert.glsl:
      // x2, 128 = 1.0) or, on a p2 range where X4 leaves it unused, DATA1.w (1x).
      if (extended)
        v.color[3] = source.vertexData1.empty() ? 0 : source.vertexData1[index][3];
      else
        v.color[3] = source.colors.empty() ? 255 : source.colors[index][3];
      if (!data.empty()) v.data = data[index];
    }
  };
  if (anyPlain) appendBlock(false);
  if (anyExtended) appendBlock(true);
  if (!part.vertices.empty()) {
    part.boundsMin = part.boundsMax = part.vertices.front().position;
    for (const ArenaVertex& v : part.vertices) {
      for (std::size_t axis = 0; axis < 3; ++axis) {
        part.boundsMin[axis] = std::min(part.boundsMin[axis], v.position[axis]);
        part.boundsMax[axis] = std::max(part.boundsMax[axis], v.position[axis]);
      }
    }
  }
  const auto extendedBase = static_cast<std::uint32_t>(anyPlain ? vertices : 0);
  part.indices = source.indices;
  for (std::size_t i = 0; i < ranges->size(); ++i)
    part.ranges.push_back(
        {(*ranges)[i].firstIndex, (*ranges)[i].indexCount, isExtended(i) ? extendedBase : 0U});
  return part;
}

std::optional<::Model> modelFromXmf(const XmfMesh& source, bool rawColors, bool tangentFrame) {
  const auto prepared = prepareXmfModel(source, rawColors, tangentFrame);
  return prepared ? uploadXmfModel(*prepared) : std::nullopt;
}

}  // namespace x4sb::editor

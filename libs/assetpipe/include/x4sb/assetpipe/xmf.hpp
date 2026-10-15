#pragma once
// X4 .xmf ("XUMF" / XuMeshFile) mesh reader (spec §10.1, the #1 unknown — now
// retired). Render-free. The container is: a 0x40 header, then N chunk
// descriptors (type, compressed size, element count, element stride), then the
// chunk buffers as contiguous zlib streams. Vertex chunks carry position as
// float3 at offset 0; index chunks are uint16/uint32. Validated against real
// base-game and DLC meshes (the AABB matches the component XML's <size>).
#include "x4sb/data/math.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace x4sb {

// One material's index-range within the part mesh. X4 parts are multi-material
// (hull plates / pattern / fx each render with their own texture set); losing
// this collapses the whole part onto one material and it reads as flat clay.
struct XmfSubmesh {
  std::uint32_t firstIndex{0};
  std::uint32_t indexCount{0};
  std::string materialRef;  // "collection.material", resolved via MaterialLibrary
};

struct XmfMesh {
  std::vector<Vec3> positions;   // one per vertex, X4-local space
  std::vector<Vec3> normals;     // one per vertex
  std::vector<Vec2> texcoords;   // one per vertex, UV map 0
  std::vector<Vec2> texcoords1;  // UV map 1 (planet clouds, ring overlays); may be empty
  std::vector<Vec2> texcoords2;  // UV map 2 (aurora third layer); may be empty
  std::vector<Vec2> texcoords3;  // UV map 3 (complex_unlit alpha3 mask); may be empty
  // TANGENT[0], decoded like NORMAL. X4 packs the bitangent's handedness into the
  // packed NORMAL's w byte (common_vert.glsl ReconstructTangentFrameFromColorSign):
  // bitangent = cross(normal, tangent) * sign. Both empty without a tangent.
  std::vector<Vec3> tangents;
  std::vector<float> bitangentSigns;  // +1 / -1
  // Per-vertex RGBA tint. X4 hull diffuse maps are greyscale; the module's
  // visible color is painted here (COLOR[0] in the vertex declaration) and
  // multiplied over the texture. May be empty (not all meshes carry it).
  std::vector<std::array<std::uint8_t, 4>> colors;
  // COLOR[1], X4's DATA1 stream, RGBA like `colors` but linear: (paint-mod mask,
  // ao, dirt amount, p2_multimat dirt amount) on hull meshes (common_vert.glsl
  // GetVertexDamageDetail). May be empty.
  std::vector<std::array<std::uint8_t, 4>> vertexData1;
  // COLOR[2], X4's DATA2, encoded like DATA1: p2_multimat reads (smoothness, metalness,
  // detail-array layer / 255, unused). May be empty.
  std::vector<std::array<std::uint8_t, 4>> vertexData2;
  std::vector<std::uint32_t> indices;  // triangle list (3 per face)
  std::vector<XmfSubmesh> submeshes;   // material table; may be empty (old/simple files)
  AABB bounds{};                       // derived from positions
};

// Parse a whole .xmf file's bytes. Returns nullopt if the data is not a valid
// XUMF mesh (bad magic, truncated, or a buffer fails to decompress).
[[nodiscard]] std::optional<XmfMesh> parseXmf(const std::string& bytes);

}  // namespace x4sb

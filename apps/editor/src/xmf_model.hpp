#pragma once

#include "raylib.h"
#include "x4sb/assetpipe/xmf.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace x4sb::editor {

// Validated, display-space vertex data for one XMF range. Plain vectors so it
// can be built on a worker thread and discarded there without a GL context.
struct PreparedMesh {
  std::vector<float> vertices;
  std::vector<float> normals;
  std::vector<float> texcoords;
  std::vector<unsigned char> colors;
  std::vector<float> tangents;    // xyz + bitangent sign; only with `tangentFrame`
  std::vector<float> texcoords2;  // UV map 1; only with `tangentFrame` or `uv1`
  // UV maps 2 and 3 as (u2, v2, u3, v3), with `tangentFrame` or whenever the mesh
  // has UV map 2 (zero where a map is missing). raylib has no third UV attribute, so
  // they ride the vec4 bone-weight slot (shader input `vertexBoneWeights`); nothing
  // here is skinned.
  std::vector<float> texcoords3;
  std::vector<unsigned short> indices;  // empty = deindexed (> u16 vertices)
};

// One mesh per XMF range, in table order (one default range when absent), for the
// sky and planet meshes; station parts use prepareStationPart. Positions and normals
// are Z-reflected into display space. `tangentFrame` adds tangents and UV maps 1-3
// (the planet shaders' inputs; zero when the mesh lacks them); UV map 2 also comes
// whenever present. `uv1` adds UV map 1 alone. Thread-safe: no GL, no raylib state.
[[nodiscard]] std::optional<std::vector<PreparedMesh>> prepareXmfModel(const XmfMesh& source,
                                                                       bool rawColors = false,
                                                                       bool tangentFrame = false,
                                                                       bool uv1 = false);
// Render-thread upload with raylib-owned buffers; release with UnloadModel.
[[nodiscard]] std::optional<::Model> uploadXmfModel(const std::vector<PreparedMesh>& meshes);
[[nodiscard]] std::optional<::Model> modelFromXmf(const XmfMesh& source, bool rawColors = false,
                                                  bool tangentFrame = false);

// One vertex of the station geometry arena (design 2026-10-14 §4.3): every stream
// always present, zero where the XMF lacks it. Attribute locations are raylib's.
struct ArenaVertex {
  std::array<float, 3> position;
  // INT_2_10_10_10_REV snorm xyz: X4 ships 8-bit normals, so 10 bits lose nothing.
  std::uint32_t normal;
  std::array<float, 2> uv0;
  std::array<float, 2> uv1;   // p1_multimat's dirt UV, complex_unlit's IO_uv1
  std::array<float, 4> uv23;  // UV maps 2 and 3: the hull paint UV, complex_unlit's IO_uv2/3
  std::array<std::uint8_t, 4> color;  // COLOR0; alpha is the dirt amount, never opacity
  std::array<std::uint8_t, 4> data;   // DATA1, or DATA2 in an extended block; unnormalized
};
static_assert(sizeof(ArenaVertex) == 56);
// The visibility resolve reads these as words (kResolveMain's fetchCorner).
static_assert(offsetof(ArenaVertex, normal) == 12 && offsetof(ArenaVertex, uv0) == 16 &&
              offsetof(ArenaVertex, uv1) == 24 && offsetof(ArenaVertex, uv23) == 32 &&
              offsetof(ArenaVertex, color) == 48 && offsetof(ArenaVertex, data) == 52);

[[nodiscard]] std::uint32_t packSnorm10(float x, float y, float z);

// One XMF range of a PreparedPart: its triangles in `indices`, which index the
// vertex block starting at `baseVertex`.
struct PreparedRange {
  std::uint32_t firstIndex{0};
  std::uint32_t indexCount{0};
  std::uint32_t baseVertex{0};
};

// A station part for the shared arena: the part's vertices once (not once per range),
// 32-bit indices relative to a block, so nothing is de-indexed.
struct PreparedPart {
  std::vector<ArenaVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<PreparedRange> ranges;
  bool hasNormals{false};  // else the normals are zero and the shaders fall back
  // Part-local box of the positions, the occlusion test's bounds (hiz spec §2.1).
  std::array<float, 3> boundsMin{};
  std::array<float, 3> boundsMax{};
};

// prepareXmfModel's validation, Z reflection and color policy for a station part.
// `extendedVertexData[i]` marks range i as running an X4 USEEXTENDEDVDATA shader
// (p2_multimat): it reads DATA2 where others read DATA1, and DATA1 only for its .w dirt
// amount, so its vertices carry DATA2 in the data slot and DATA1.w as COLOR0 alpha
// (unused there). A part mixing both kinds gets a second vertex block for them.
// Thread-safe: no GL, no raylib state.
[[nodiscard]] std::optional<PreparedPart> prepareStationPart(
    const XmfMesh& source, const std::vector<bool>& extendedVertexData = {});

}  // namespace x4sb::editor

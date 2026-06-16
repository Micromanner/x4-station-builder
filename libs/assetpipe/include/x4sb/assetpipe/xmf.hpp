#pragma once
// X4 .xmf ("XUMF" / XuMeshFile) mesh reader (spec §10.1, the #1 unknown — now
// retired). Render-free. The container is: a 0x40 header, then N chunk
// descriptors (type, compressed size, element count, element stride), then the
// chunk buffers as contiguous zlib streams. Vertex chunks carry position as
// float3 at offset 0; index chunks are uint16/uint32. Validated against real
// base-game and DLC meshes (the AABB matches the component XML's <size>).
#include "x4sb/data/math.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace x4sb {

struct XmfMesh {
  std::vector<Vec3> positions;         // one per vertex, X4-local space
  std::vector<std::uint32_t> indices;  // triangle list (3 per face)
  AABB bounds{};                       // derived from positions
};

// Parse a whole .xmf file's bytes. Returns nullopt if the data is not a valid
// XUMF mesh (bad magic, truncated, or a buffer fails to decompress).
[[nodiscard]] std::optional<XmfMesh> parseXmf(const std::string& bytes);

// The vertex count of a XUMF mesh, read straight from the first vertex chunk's
// descriptor (no decompression), or nullopt if the bytes are not a valid XUMF
// mesh. Lets the converter pick a LOD that fits a vertex budget before paying to
// inflate the buffers.
[[nodiscard]] std::optional<std::uint32_t> xmfVertexCount(const std::string& bytes);

}  // namespace x4sb

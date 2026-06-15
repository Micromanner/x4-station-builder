#include "x4sb/assetpipe/xmf.hpp"

#include <sinfl.h>

#include <cstring>
#include <limits>

namespace x4sb {
namespace {

// Little-endian reads (the X4 target is x86/x64; .xmf is little-endian).
template <typename T>
T readLE(const char* p) {
  T v{};
  std::memcpy(&v, p, sizeof(v));
  return v;
}
template <typename T>
T readLE(const std::string& b, std::size_t off) {
  return readLE<T>(b.data() + off);
}

// Chunk descriptor fields, by u32 index within the descriptor (validated against
// real meshes): [0]=type (0 vertex, 30 index), [6]=compressed bytes,
// [7]=element count, [8]=element stride/size.
constexpr std::size_t kTypeOff = 0;
constexpr std::size_t kCompOff = 6 * 4;
constexpr std::size_t kCountOff = 7 * 4;
constexpr std::size_t kStrideOff = 8 * 4;
constexpr std::uint32_t kTypeVertex = 0;
constexpr std::uint32_t kTypeIndex = 30;

bool inflateChunk(const std::string& bytes, std::size_t streamOff, std::uint32_t compSize,
                  std::size_t decompSize, std::string& out) {
  if (decompSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
  if (streamOff + compSize > bytes.size()) return false;
  out.assign(decompSize, '\0');
  const int written = zsinflate(out.data(), static_cast<int>(decompSize), bytes.data() + streamOff,
                                static_cast<int>(compSize));
  return written >= 0 && static_cast<std::size_t>(written) == decompSize;
}

}  // namespace

std::optional<XmfMesh> parseXmf(const std::string& bytes) {
  if (bytes.size() < 0x40 || std::memcmp(bytes.data(), "XUMF", 4) != 0) return std::nullopt;

  const std::size_t headerSize = readLE<std::uint8_t>(bytes, 6);
  const std::size_t numChunks = readLE<std::uint8_t>(bytes, 8);
  const std::size_t descSize = readLE<std::uint8_t>(bytes, 9);
  if (descSize < kStrideOff + 4) return std::nullopt;

  const std::size_t descEnd = headerSize + numChunks * descSize;
  if (descEnd > bytes.size()) return std::nullopt;

  // Per-chunk descriptor fields and the total compressed size (to locate the
  // contiguous stream region, which ends at EOF).
  struct Chunk {
    std::uint32_t type, comp, count, stride;
  };
  std::vector<Chunk> chunks;
  chunks.reserve(numChunks);
  std::size_t totalComp = 0;
  for (std::size_t i = 0; i < numChunks; ++i) {
    const std::size_t d = headerSize + i * descSize;
    const Chunk c{readLE<std::uint32_t>(bytes, d + kTypeOff),
                  readLE<std::uint32_t>(bytes, d + kCompOff),
                  readLE<std::uint32_t>(bytes, d + kCountOff),
                  readLE<std::uint32_t>(bytes, d + kStrideOff)};
    totalComp += c.comp;
    chunks.push_back(c);
  }
  if (totalComp > bytes.size() || bytes.size() - totalComp < descEnd) return std::nullopt;

  std::size_t streamOff = bytes.size() - totalComp;  // streams are contiguous to EOF

  XmfMesh mesh;
  bool haveVertices = false;
  std::string buf;
  for (const Chunk& c : chunks) {
    const std::size_t decompSize = static_cast<std::size_t>(c.count) * c.stride;
    if (c.type == kTypeVertex && !haveVertices) {
      if (c.stride < 12) return std::nullopt;  // need at least a float3 position
      if (!inflateChunk(bytes, streamOff, c.comp, decompSize, buf)) return std::nullopt;
      mesh.positions.reserve(c.count);
      for (std::size_t v = 0; v < c.count; ++v) {
        const char* p = buf.data() + v * c.stride;
        mesh.positions.push_back({static_cast<double>(readLE<float>(p)),
                                  static_cast<double>(readLE<float>(p + 4)),
                                  static_cast<double>(readLE<float>(p + 8))});
      }
      haveVertices = true;
    } else if (c.type == kTypeIndex) {
      if (c.stride != 2 && c.stride != 4) return std::nullopt;
      if (!inflateChunk(bytes, streamOff, c.comp, decompSize, buf)) return std::nullopt;
      mesh.indices.reserve(mesh.indices.size() + c.count);
      for (std::size_t k = 0; k < c.count; ++k) {
        const char* p = buf.data() + k * c.stride;
        mesh.indices.push_back(c.stride == 2 ? static_cast<std::uint32_t>(readLE<std::uint16_t>(p))
                                             : readLE<std::uint32_t>(p));
      }
    }
    streamOff += c.comp;
  }

  if (!haveVertices || mesh.positions.empty()) return std::nullopt;

  mesh.bounds.min = mesh.bounds.max = mesh.positions.front();
  for (const Vec3& v : mesh.positions) expand(mesh.bounds, v);
  return mesh;
}

}  // namespace x4sb

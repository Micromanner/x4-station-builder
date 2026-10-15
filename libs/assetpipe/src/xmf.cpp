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

float halfToFloat(std::uint16_t h) {
  std::uint32_t sign = (h >> 15) & 0x00000001;
  std::uint32_t exp = (h >> 10) & 0x0000001F;
  std::uint32_t mant = h & 0x000003FF;
  if (exp == 0) {
    if (mant == 0) return sign ? -0.0f : 0.0f;
    while ((mant & 0x00000400) == 0) {
      mant <<= 1;
      exp--;
    }
    exp++;
    mant &= ~0x00000400u;
  } else if (exp == 31) {
    if (mant == 0) return sign ? -1e38f : 1e38f;  // infinity approximation
    return 0.0f;                                  // nan approximation
  }
  exp = exp + (127 - 15);
  mant = mant << 13;
  std::uint32_t bits = (sign << 31) | (exp << 23) | mant;
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
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
    std::size_t descOffset;
  };
  std::vector<Chunk> chunks;
  chunks.reserve(numChunks);
  std::size_t totalComp = 0;
  for (std::size_t i = 0; i < numChunks; ++i) {
    const std::size_t d = headerSize + i * descSize;
    const Chunk c{readLE<std::uint32_t>(bytes, d + kTypeOff),
                  readLE<std::uint32_t>(bytes, d + kCompOff),
                  readLE<std::uint32_t>(bytes, d + kCountOff),
                  readLE<std::uint32_t>(bytes, d + kStrideOff), d};
    if (c.type == kTypeVertex) {
      // ...
    }
    totalComp += c.comp;
    chunks.push_back(c);
  }
  if (totalComp > bytes.size() || bytes.size() - totalComp < descEnd) return std::nullopt;

  std::size_t streamOff = bytes.size() - totalComp;  // streams are contiguous to EOF

  // The material table sits between the chunk descriptors and the streams:
  // header[10] = entry count, header[11] = entry size (0x88); each entry is
  // u32 firstIndex, u32 indexCount, char[128] "collection.material" (validated
  // against real base-game files). Every field is untrusted — bounds-check all.
  std::vector<XmfSubmesh> submeshes;
  {
    const std::size_t numMaterials = readLE<std::uint8_t>(bytes, 10);
    const std::size_t matEntrySize = readLE<std::uint8_t>(bytes, 11);
    if (numMaterials > 0 && matEntrySize >= 9 &&
        descEnd + numMaterials * matEntrySize <= streamOff) {
      for (std::size_t i = 0; i < numMaterials; ++i) {
        const std::size_t e = descEnd + i * matEntrySize;
        XmfSubmesh sm;
        sm.firstIndex = readLE<std::uint32_t>(bytes, e);
        sm.indexCount = readLE<std::uint32_t>(bytes, e + 4);
        const std::size_t nameCap = matEntrySize - 8;
        const char* name = bytes.data() + e + 8;
        const std::size_t nameLen = strnlen(name, nameCap);
        sm.materialRef.assign(name, nameLen);
        submeshes.push_back(std::move(sm));
      }
    }
  }

  XmfMesh mesh;
  bool haveVertices = false;
  std::string buf;
  for (const Chunk& c : chunks) {
    const std::size_t decompSize = static_cast<std::size_t>(c.count) * c.stride;
    if (c.type == kTypeVertex && !haveVertices) {
      if (c.stride < 12) return std::nullopt;  // need at least a float3 position
      if (!inflateChunk(bytes, streamOff, c.comp, decompSize, buf)) return std::nullopt;

      std::uint32_t posOff = 0xFFFFFFFF;
      std::uint32_t normOff = 0xFFFFFFFF;
      std::uint32_t normType = 0;
      std::uint32_t uvOff = 0xFFFFFFFF;
      std::uint32_t uvType = 0;
      std::uint32_t colOff = 0xFFFFFFFF;
      std::uint32_t colType = 0;
      std::uint32_t data1Off = 0xFFFFFFFF;
      std::uint32_t data1Type = 0;
      std::uint32_t data2Off = 0xFFFFFFFF;
      std::uint32_t data2Type = 0;
      std::uint32_t tanOff = 0xFFFFFFFF;
      std::uint32_t tanType = 0;
      std::uint32_t uv1Off = 0xFFFFFFFF;
      std::uint32_t uv1Type = 0;
      std::uint32_t uv2Off = 0xFFFFFFFF;
      std::uint32_t uv2Type = 0;
      std::uint32_t uv3Off = 0xFFFFFFFF;
      std::uint32_t uv3Type = 0;

      // Extract elements from descriptor at c.descOffset + 60
      if (descSize >= 60) {
        // The element list lives inside the descriptor; a hostile count past it
        // would read beyond the file.
        const std::size_t numElements = std::min<std::size_t>(
            readLE<std::uint32_t>(bytes, c.descOffset + 56), (descSize - 60) / 8);
        std::uint32_t currentOff = 0;
        for (std::size_t e = 0; e < numElements; ++e) {
          std::uint32_t type = readLE<std::uint32_t>(bytes, c.descOffset + 60 + e * 8);
          std::uint32_t usageVal = readLE<std::uint32_t>(bytes, c.descOffset + 60 + e * 8 + 4);
          std::uint32_t usage = usageVal & 0xFF;
          std::uint32_t usageIdx = (usageVal >> 8) & 0xFF;

          if (usage == 0) {
            posOff = currentOff;
          } else if (usage == 3 && usageIdx == 0) {
            normOff = currentOff;
            normType = type;
          } else if (usage == 5 && usageIdx == 0) {
            uvOff = currentOff;
            uvType = type;
          } else if (usage == 10 && usageIdx == 0) {
            colOff = currentOff;
            colType = type;
          } else if (usage == 10 && usageIdx == 1) {
            data1Off = currentOff;
            data1Type = type;
          } else if (usage == 10 && usageIdx == 2) {
            data2Off = currentOff;
            data2Type = type;
          } else if (usage == 6 && usageIdx == 0) {
            tanOff = currentOff;
            tanType = type;
          } else if (usage == 5 && usageIdx == 1) {
            uv1Off = currentOff;
            uv1Type = type;
          } else if (usage == 5 && usageIdx == 2) {
            uv2Off = currentOff;
            uv2Type = type;
          } else if (usage == 5 && usageIdx == 3) {
            uv3Off = currentOff;
            uv3Type = type;
          }

          std::uint32_t size = 0;
          switch (type) {
            case 0:
              size = 4;
              break;
            case 1:
              size = 8;
              break;
            case 2:
              size = 12;
              break;
            case 3:
              size = 16;
              break;
            case 4:
            case 5:
            case 6:
            case 8:
            case 9:
            case 11:
            case 13:
            case 14:
            case 15:
              size = 4;
              break;
            case 7:
            case 10:
            case 12:
            case 16:
              size = 8;
              break;
          }
          currentOff += size;
        }
      } else {
        posOff = 0;
        if (c.stride >= 24) {
          normOff = 12;
          normType = 2;
        }
        if (c.stride >= 48) {
          uvOff = 40;
          uvType = 1;
        }
      }

      if (posOff == 0xFFFFFFFF) return std::nullopt;  // MUST have position
      // Every element read must fit the vertex record, or the last vertex reads
      // past the buffer: a position that doesn't fit rejects the mesh, a normal
      // or UV that doesn't is dropped.
      if (std::size_t{posOff} + 12 > c.stride) return std::nullopt;
      if (normOff != 0xFFFFFFFF && std::size_t{normOff} + (normType == 2 ? 12U : 4U) > c.stride)
        normOff = 0xFFFFFFFF;
      if (uvOff != 0xFFFFFFFF && std::size_t{uvOff} + (uvType == 1 ? 8U : 4U) > c.stride)
        uvOff = 0xFFFFFFFF;

      // Only extract COLOR when it's D3DCOLOR (type 4 — every sampled real mesh)
      // AND its 4 bytes fit the declared stride: a hostile declaration whose
      // offsets exceed the stride would otherwise read past the vertex buffer.
      const bool haveColor = colOff != 0xFFFFFFFF && colType == 4 && colOff + 4 <= c.stride;
      const bool haveData1 = data1Off != 0xFFFFFFFF && data1Type == 4 && data1Off + 4 <= c.stride;
      const bool haveData2 = data2Off != 0xFFFFFFFF && data2Type == 4 && data2Off + 4 <= c.stride;
      // Same hostile-declaration guard for the tangent frame and UV maps 1-3: only
      // the encodings real meshes use, and only when they fit the stride.
      const bool haveTangent = tanOff != 0xFFFFFFFF && ((tanType == 4 && tanOff + 4 <= c.stride) ||
                                                        (tanType == 2 && tanOff + 12 <= c.stride));
      const bool packedNormal = normOff != 0xFFFFFFFF && normType != 2 && normOff + 4 <= c.stride;
      const auto uvFits = [&c](std::uint32_t off, std::uint32_t type) {
        return off != 0xFFFFFFFF &&
               ((type == 1 && off + 8 <= c.stride) || (type == 15 && off + 4 <= c.stride) ||
                (type == 16 && off + 8 <= c.stride));
      };
      const bool haveUv1 = uvFits(uv1Off, uv1Type);
      const bool haveUv2 = uvFits(uv2Off, uv2Type);
      const bool haveUv3 = uvFits(uv3Off, uv3Type);

      mesh.positions.reserve(c.count);
      if (normOff != 0xFFFFFFFF) mesh.normals.reserve(c.count);
      if (uvOff != 0xFFFFFFFF) mesh.texcoords.reserve(c.count);
      if (haveColor) mesh.colors.reserve(c.count);
      if (haveData1) mesh.vertexData1.reserve(c.count);
      if (haveData2) mesh.vertexData2.reserve(c.count);
      if (haveTangent) {
        mesh.tangents.reserve(c.count);
        mesh.bitangentSigns.reserve(c.count);
      }
      if (haveUv1) mesh.texcoords1.reserve(c.count);
      if (haveUv2) mesh.texcoords2.reserve(c.count);
      if (haveUv3) mesh.texcoords3.reserve(c.count);

      for (std::size_t v = 0; v < c.count; ++v) {
        const char* p = buf.data() + v * c.stride;

        mesh.positions.push_back({static_cast<double>(readLE<float>(p + posOff)),
                                  static_cast<double>(readLE<float>(p + posOff + 4)),
                                  static_cast<double>(readLE<float>(p + posOff + 8))});

        if (normOff != 0xFFFFFFFF) {
          if (normType == 2) {
            mesh.normals.push_back({static_cast<double>(readLE<float>(p + normOff)),
                                    static_cast<double>(readLE<float>(p + normOff + 4)),
                                    static_cast<double>(readLE<float>(p + normOff + 8))});
          } else {
            // Packed normals are D3DCOLOR like the vertex colours: BGRA in memory,
            // so x is byte 2 and z byte 0, encoded 1 - 2*b/255. Verified against
            // the geometric face normals of every baked module (100% agreement;
            // the byte-order reading was mirrored and drew camera-dependent
            // shading seams across flat hull faces).
            const auto decode = [p, normOff](std::size_t byte) {
              return 1.0 - 2.0 * (static_cast<unsigned char>(p[normOff + byte]) / 255.0);
            };
            mesh.normals.push_back({decode(2), decode(1), decode(0)});
          }
        }

        const std::size_t base = v * c.stride;
        if (haveTangent) {
          if (tanType == 2) {
            mesh.tangents.push_back({static_cast<double>(readLE<float>(buf, base + tanOff)),
                                     static_cast<double>(readLE<float>(buf, base + tanOff + 4)),
                                     static_cast<double>(readLE<float>(buf, base + tanOff + 8))});
          } else {
            const auto decode = [&buf, base, tanOff](std::size_t byte) {
              return 1.0 - 2.0 * (static_cast<unsigned char>(buf[base + tanOff + byte]) / 255.0);
            };
            mesh.tangents.push_back({decode(2), decode(1), decode(0)});
          }
          // X4 reads w as 2*b/255 - 1 and tests >= 0, i.e. byte >= 128.
          const bool positive =
              !packedNormal || static_cast<unsigned char>(buf[base + normOff + 3]) >= 128;
          mesh.bitangentSigns.push_back(positive ? 1.0F : -1.0F);
        }

        const auto readUv = [&buf, base](std::uint32_t off, std::uint32_t type) -> Vec2 {
          if (type == 1)
            return {static_cast<double>(readLE<float>(buf, base + off)),
                    static_cast<double>(readLE<float>(buf, base + off + 4))};
          return {static_cast<double>(halfToFloat(readLE<std::uint16_t>(buf, base + off))),
                  static_cast<double>(halfToFloat(readLE<std::uint16_t>(buf, base + off + 2)))};
        };
        if (haveUv1) mesh.texcoords1.push_back(readUv(uv1Off, uv1Type));
        if (haveUv2) mesh.texcoords2.push_back(readUv(uv2Off, uv2Type));
        if (haveUv3) mesh.texcoords3.push_back(readUv(uv3Off, uv3Type));

        if (haveColor) {
          // D3DCOLOR is packed ARGB = B,G,R,A in memory; store as RGBA.
          const auto b = static_cast<std::uint8_t>(p[colOff + 0]);
          const auto g = static_cast<std::uint8_t>(p[colOff + 1]);
          const auto r = static_cast<std::uint8_t>(p[colOff + 2]);
          const auto a = static_cast<std::uint8_t>(p[colOff + 3]);
          mesh.colors.push_back({r, g, b, a});
        }
        // DATA streams are D3DCOLOR too: BGRA in memory, stored RGBA.
        const auto bgra = [p](std::uint32_t off) -> std::array<std::uint8_t, 4> {
          const auto at = [p, off](std::size_t byte) {
            return static_cast<std::uint8_t>(p[off + byte]);
          };
          return {at(2), at(1), at(0), at(3)};
        };
        if (haveData1) mesh.vertexData1.push_back(bgra(data1Off));
        if (haveData2) mesh.vertexData2.push_back(bgra(data2Off));

        if (uvOff != 0xFFFFFFFF) {
          if (uvType == 1) {  // FLOAT2
            mesh.texcoords.push_back({static_cast<double>(readLE<float>(p + uvOff)),
                                      static_cast<double>(readLE<float>(p + uvOff + 4))});
          } else if (uvType == 15 || uvType == 16) {  // FLOAT16_2 or FLOAT16_4
            mesh.texcoords.push_back(
                {static_cast<double>(halfToFloat(readLE<std::uint16_t>(p + uvOff))),
                 static_cast<double>(halfToFloat(readLE<std::uint16_t>(p + uvOff + 2)))});
          }
        }
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

  // Adopt the material table only when every range lies inside the index buffer
  // (a corrupt table degrades to the single-material fallback, not a crash).
  bool tableValid = !submeshes.empty();
  for (const XmfSubmesh& sm : submeshes) {
    if (sm.materialRef.empty() || sm.firstIndex > mesh.indices.size() ||
        sm.indexCount > mesh.indices.size() - sm.firstIndex)
      tableValid = false;
  }
  if (tableValid) mesh.submeshes = std::move(submeshes);

  mesh.bounds.min = mesh.bounds.max = mesh.positions.front();
  for (const Vec3& v : mesh.positions) expand(mesh.bounds, v);
  return mesh;
}

}  // namespace x4sb

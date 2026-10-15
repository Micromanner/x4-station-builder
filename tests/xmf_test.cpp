#include "x4sb/assetpipe/xmf.hpp"

#include "x4sb/assetpipe/material_library.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace x4sb;

// A self-authored synthetic XUMF mesh (NOT extracted game data — spec §12): a
// tetrahedron with 4 float3 vertices (0,0,0)(10,0,0)(0,10,0)(0,0,10) and 12
// uint16 indices, with both buffers real zlib streams. Exercises the actual
// sinfl decompression path the real meshes use.
namespace {
const unsigned char kTetraXmf[] = {
    0x58, 0x55, 0x4d, 0x46, 0x03, 0x00, 0x40, 0x00, 0x02, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x78, 0xda, 0x63, 0x60, 0x40, 0x06, 0x0a, 0x8e,
    0x0c, 0x04, 0xf8, 0x00, 0x13, 0xe3, 0x01, 0x24, 0x78, 0xda, 0x63, 0x60, 0x60, 0x64, 0x60, 0x62,
    0x60, 0x00, 0x92, 0xcc, 0x40, 0x92, 0x09, 0x48, 0x32, 0x82, 0x49, 0x00, 0x00, 0xd0, 0x00, 0x13,
};

std::string fixture() {
  return std::string(reinterpret_cast<const char*>(kTetraXmf), sizeof(kTetraXmf));
}

// The tetra fixture with a material table spliced between the chunk descriptors
// and the streams (header[10] = entry count, header[11] = entry size; entry =
// u32 firstIndex, u32 indexCount, NUL-padded name).
std::string fixtureWithMaterial(std::uint32_t firstIndex, std::uint32_t indexCount) {
  std::string f = fixture();
  f[10] = 1;
  f[11] = 17;  // 8 range bytes + 9 name bytes
  std::string entry(17, '\0');
  std::memcpy(entry.data(), &firstIndex, 4);
  std::memcpy(entry.data() + 4, &indexCount, 4);
  std::memcpy(entry.data() + 8, "test.mat", 8);
  const std::size_t descEnd = 0x40 + 2 * 0x24;  // header + 2 descriptors of 0x24
  f.insert(descEnd, entry);
  return f;
}
// --- Declaration-driven fixtures ---------------------------------------------
// The tetra blob above has descSize 0x24 (< 60), so it exercises only the
// stride-fallback path. Real meshes ship a 188-byte descriptor with a D3D9
// vertex declaration; these helpers synthesize one so declaration-driven
// extraction (COLOR etc.) is testable. Streams are zlib "stored" blocks —
// valid input for the real inflate path with no deflate implementation needed.

std::string zlibStored(const std::string& raw) {
  std::string z;
  z += '\x78';
  z += '\x01';
  z += '\x01';  // final stored block
  const auto len = static_cast<std::uint16_t>(raw.size());
  const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
  z += static_cast<char>(len & 0xFF);
  z += static_cast<char>(len >> 8);
  z += static_cast<char>(nlen & 0xFF);
  z += static_cast<char>(nlen >> 8);
  z += raw;
  std::uint32_t a = 1, b = 0;
  for (const char c : raw) {
    a = (a + static_cast<unsigned char>(c)) % 65521;
    b = (b + a) % 65521;
  }
  const std::uint32_t adler = (b << 16) | a;
  z += static_cast<char>((adler >> 24) & 0xFF);
  z += static_cast<char>((adler >> 16) & 0xFF);
  z += static_cast<char>((adler >> 8) & 0xFF);
  z += static_cast<char>(adler & 0xFF);
  return z;
}

struct DeclElement {
  std::uint32_t type;
  std::uint32_t usage;
  std::uint32_t usageIdx = 0;
};

void putU32(std::string& s, std::size_t off, std::uint32_t v) {
  std::memcpy(s.data() + off, &v, 4);
}

std::string makeDeclXmf(const std::vector<DeclElement>& elems, const std::string& vertexData,
                        std::uint32_t vertexCount, std::uint32_t stride,
                        const std::vector<std::uint16_t>& indices) {
  constexpr std::size_t kDescSize = 188;
  std::string f(0x40, '\0');
  std::memcpy(f.data(), "XUMF", 4);
  f[4] = 3;
  f[6] = 0x40;
  f[8] = 2;  // vertex + index chunks
  f[9] = static_cast<char>(kDescSize);

  const std::string vz = zlibStored(vertexData);
  std::string iraw(indices.size() * 2, '\0');
  std::memcpy(iraw.data(), indices.data(), iraw.size());
  const std::string iz = zlibStored(iraw);

  std::string dv(kDescSize, '\0');  // vertex chunk descriptor
  putU32(dv, 0, 0);
  putU32(dv, 24, static_cast<std::uint32_t>(vz.size()));
  putU32(dv, 28, vertexCount);
  putU32(dv, 32, stride);
  putU32(dv, 56, static_cast<std::uint32_t>(elems.size()));
  for (std::size_t e = 0; e < elems.size(); ++e) {
    putU32(dv, 60 + e * 8, elems[e].type);
    putU32(dv, 60 + e * 8 + 4, elems[e].usage | (elems[e].usageIdx << 8));
  }

  std::string di(kDescSize, '\0');  // index chunk descriptor
  putU32(di, 0, 30);
  putU32(di, 24, static_cast<std::uint32_t>(iz.size()));
  putU32(di, 28, static_cast<std::uint32_t>(indices.size()));
  putU32(di, 32, 2);

  return f + dv + di + vz + iz;
}

void putF32(std::string& s, std::size_t off, float v) { std::memcpy(s.data() + off, &v, 4); }

// One triangle, stride 16: POSITION float3 + COLOR[0] as D3DCOLOR (type 4,
// BGRA byte order). Vertex 0 carries B=10 G=20 R=30 A=40.
std::string triangleWithColors() {
  std::string vd(3 * 16, '\0');
  const std::array<std::array<float, 3>, 3> pos{{{0, 0, 0}, {10, 0, 0}, {0, 10, 0}}};
  const std::array<std::array<unsigned char, 4>, 3> bgra{
      {{10, 20, 30, 40}, {50, 60, 70, 80}, {128, 128, 128, 0}}};
  for (std::size_t v = 0; v < 3; ++v) {
    putF32(vd, v * 16 + 0, pos[v][0]);
    putF32(vd, v * 16 + 4, pos[v][1]);
    putF32(vd, v * 16 + 8, pos[v][2]);
    std::memcpy(vd.data() + v * 16 + 12, bgra[v].data(), 4);
  }
  return vd;
}

}  // namespace

TEST_CASE("parseXmf extracts D3DCOLOR vertex colors, swizzling BGRA bytes to RGBA") {
  const auto mesh =
      parseXmf(makeDeclXmf({{2, 0}, {4, 10}}, triangleWithColors(), 3, 16, {0, 1, 2}));
  REQUIRE(mesh.has_value());
  REQUIRE(mesh->positions.size() == 3);
  CHECK(mesh->positions[1].x == doctest::Approx(10));

  REQUIRE(mesh->colors.size() == 3);
  CHECK(mesh->colors[0] == std::array<std::uint8_t, 4>{30, 20, 10, 40});
  CHECK(mesh->colors[1] == std::array<std::uint8_t, 4>{70, 60, 50, 80});
  CHECK(mesh->colors[2] == std::array<std::uint8_t, 4>{128, 128, 128, 0});
}

TEST_CASE("parseXmf extracts COLOR[1] as vertexData1, apart from the tint") {
  const auto mesh =
      parseXmf(makeDeclXmf({{2, 0}, {4, 10 | (1 << 8)}}, triangleWithColors(), 3, 16, {0, 1, 2}));
  REQUIRE(mesh.has_value());
  CHECK(mesh->colors.empty());
  REQUIRE(mesh->vertexData1.size() == 3);
  CHECK(mesh->vertexData1[0] == std::array<std::uint8_t, 4>{30, 20, 10, 40});
  CHECK(mesh->vertexData1[2] == std::array<std::uint8_t, 4>{128, 128, 128, 0});
}

TEST_CASE("parseXmf extracts COLOR[2] as vertexData2, apart from DATA1") {
  const auto mesh =
      parseXmf(makeDeclXmf({{2, 0}, {4, 10 | (2 << 8)}}, triangleWithColors(), 3, 16, {0, 1, 2}));
  REQUIRE(mesh.has_value());
  CHECK(mesh->colors.empty());
  CHECK(mesh->vertexData1.empty());
  REQUIRE(mesh->vertexData2.size() == 3);
  CHECK(mesh->vertexData2[1] == std::array<std::uint8_t, 4>{70, 60, 50, 80});
}

TEST_CASE("parseXmf decodes packed D3DCOLOR normals as BGR bytes, 1 - 2*b/255") {
  // Stride 16: POSITION float3 + NORMAL as D3DCOLOR (type 4, usage 3). Vertex 0
  // bytes B=255 G=128 R=0 -> normal x=+1 (from R), y~0 (G), z=-1 (from B).
  std::string vd(3 * 16, '\0');
  const std::array<unsigned char, 4> bgra{255, 128, 0, 0};
  for (std::size_t v = 0; v < 3; ++v) std::memcpy(vd.data() + v * 16 + 12, bgra.data(), 4);
  putF32(vd, 16 + 0, 10.0F);
  putF32(vd, 32 + 4, 10.0F);
  const auto mesh = parseXmf(makeDeclXmf({{2, 0}, {4, 3}}, vd, 3, 16, {0, 1, 2}));
  REQUIRE(mesh.has_value());
  REQUIRE(mesh->normals.size() == 3);
  CHECK(mesh->normals[0].x == doctest::Approx(1.0));
  CHECK(mesh->normals[0].y == doctest::Approx(1.0 - 2.0 * 128.0 / 255.0));
  CHECK(mesh->normals[0].z == doctest::Approx(-1.0));
}

TEST_CASE("parseXmf ignores a COLOR element whose bytes don't fit the declared stride") {
  // Hostile declaration: COLOR at offset 12 but stride is only 12 — extracting
  // it would read past each vertex record. Geometry must still parse.
  std::string vd(3 * 12, '\0');
  putF32(vd, 12 + 0, 10.0F);  // vertex 1 x
  const auto mesh = parseXmf(makeDeclXmf({{2, 0}, {4, 10}}, vd, 3, 12, {0, 1, 2}));
  REQUIRE(mesh.has_value());
  CHECK(mesh->positions.size() == 3);
  CHECK(mesh->colors.empty());
}

TEST_CASE("parseXmf reads the planet tangent frame and UV map 1") {
  // Stride 28, the planet-body declaration: POSITION float3, NORMAL and TANGENT
  // as D3DCOLOR, TEXCOORD[0] and TEXCOORD[1] as half2. Vertex 0's NORMAL w byte
  // is 200 (bitangent sign +1), the others 0 (-1).
  std::string vd(3 * 28, '\0');
  for (std::size_t v = 0; v < 3; ++v) {
    const std::array<unsigned char, 4> normal{128, 128, 0,
                                              static_cast<unsigned char>(v == 0 ? 200 : 0)};
    const std::array<unsigned char, 4> tangent{0, 128, 128, 0};
    std::memcpy(vd.data() + v * 28 + 12, normal.data(), 4);
    std::memcpy(vd.data() + v * 28 + 16, tangent.data(), 4);
    const std::array<std::uint16_t, 4> uvs{0x3800, 0x3400, 0x3C00, 0x0000};  // 0.5 0.25 1 0
    std::memcpy(vd.data() + v * 28 + 20, uvs.data(), 8);
  }
  const auto mesh =
      parseXmf(makeDeclXmf({{2, 0}, {4, 3}, {4, 6}, {15, 5, 0}, {15, 5, 1}}, vd, 3, 28, {0, 1, 2}));
  REQUIRE(mesh.has_value());
  REQUIRE(mesh->tangents.size() == 3);
  CHECK(mesh->tangents[0].x == doctest::Approx(1.0 - 2.0 * 128.0 / 255.0));
  CHECK(mesh->tangents[0].z == doctest::Approx(1.0));
  REQUIRE(mesh->bitangentSigns.size() == 3);
  CHECK(mesh->bitangentSigns[0] == 1.0F);
  CHECK(mesh->bitangentSigns[1] == -1.0F);
  REQUIRE(mesh->texcoords.size() == 3);
  CHECK(mesh->texcoords[0].x == doctest::Approx(0.5));
  REQUIRE(mesh->texcoords1.size() == 3);
  CHECK(mesh->texcoords1[0].x == doctest::Approx(1.0));
  CHECK(mesh->texcoords1[0].y == doctest::Approx(0.0));
}

TEST_CASE("parseXmf reads UV maps 2 and 3 (the aurora and alpha3 declarations)") {
  // Stride 28: POSITION float3, TEXCOORD[0..3] as half2.
  std::string vd(3 * 28, '\0');
  for (std::size_t v = 0; v < 3; ++v) {
    // uv2 = 0.5 2, uv3 = 0.25 1
    const std::array<std::uint16_t, 8> uvs{0, 0, 0x3C00, 0, 0x3800, 0x4000, 0x3400, 0x3C00};
    std::memcpy(vd.data() + v * 28 + 12, uvs.data(), 16);
  }
  const auto mesh = parseXmf(
      makeDeclXmf({{2, 0}, {15, 5, 0}, {15, 5, 1}, {15, 5, 2}, {15, 5, 3}}, vd, 3, 28, {0, 1, 2}));
  REQUIRE(mesh.has_value());
  REQUIRE(mesh->texcoords1.size() == 3);
  CHECK(mesh->texcoords1[0].x == doctest::Approx(1.0));
  REQUIRE(mesh->texcoords2.size() == 3);
  CHECK(mesh->texcoords2[2].x == doctest::Approx(0.5));
  CHECK(mesh->texcoords2[2].y == doctest::Approx(2.0));
  REQUIRE(mesh->texcoords3.size() == 3);
  CHECK(mesh->texcoords3[1].x == doctest::Approx(0.25));
  CHECK(mesh->texcoords3[1].y == doctest::Approx(1.0));
}

TEST_CASE("parseXmf ignores a TANGENT or UV1-UV3 element that overruns the stride") {
  std::string vd(3 * 12, '\0');
  const auto mesh = parseXmf(
      makeDeclXmf({{2, 0}, {4, 6}, {15, 5, 1}, {15, 5, 2}, {15, 5, 3}}, vd, 3, 12, {0, 1, 2}));
  REQUIRE(mesh.has_value());
  CHECK(mesh->tangents.empty());
  CHECK(mesh->bitangentSigns.empty());
  CHECK(mesh->texcoords1.empty());
  CHECK(mesh->texcoords2.empty());
  CHECK(mesh->texcoords3.empty());
}

TEST_CASE("parseXmf clamps a hostile element count to the descriptor") {
  std::string f = makeDeclXmf({{2, 0}}, std::string(3 * 12, char{0}), 3, 12, {0, 1, 2});
  putU32(f, 0x40 + 56, 0xFFFFFFFFU);  // vertex descriptor's element count
  // Clamped to the descriptor's 16 slots (no read past it); the zero-filled
  // slots decode as further POSITIONs that no longer fit the stride.
  CHECK_FALSE(parseXmf(f));
}

TEST_CASE("parseXmf drops a NORMAL or UV that overruns the stride, rejects such a POSITION") {
  // Declared offsets: POSITION 0, NORMAL float3 at 12, UV float2 at 24; stride 12.
  const auto mesh = parseXmf(
      makeDeclXmf({{2, 0}, {2, 3}, {1, 5}}, std::string(3 * 12, char{0}), 3, 12, {0, 1, 2}));
  REQUIRE(mesh.has_value());
  CHECK(mesh->positions.size() == 3);
  CHECK(mesh->normals.empty());
  CHECK(mesh->texcoords.empty());
  // A 4-byte COLOR first pushes POSITION to offset 4: 4 + 12 > stride 12.
  CHECK_FALSE(
      parseXmf(makeDeclXmf({{4, 10}, {2, 0}}, std::string(3 * 12, char{0}), 3, 12, {0, 1, 2})));
}

TEST_CASE("parseXmf decodes a XUMF mesh: positions, indices, AABB") {
  const auto mesh = parseXmf(fixture());
  REQUIRE(mesh.has_value());

  REQUIRE(mesh->positions.size() == 4);
  CHECK(mesh->positions[0].x == doctest::Approx(0));
  CHECK(mesh->positions[1].x == doctest::Approx(10));
  CHECK(mesh->positions[2].y == doctest::Approx(10));
  CHECK(mesh->positions[3].z == doctest::Approx(10));

  REQUIRE(mesh->indices.size() == 12);
  CHECK(mesh->indices[0] == 0u);
  CHECK(mesh->indices[11] == 3u);

  CHECK(mesh->bounds.min.x == doctest::Approx(0));
  CHECK(mesh->bounds.max.x == doctest::Approx(10));
  CHECK(mesh->bounds.max.y == doctest::Approx(10));
  CHECK(mesh->bounds.max.z == doctest::Approx(10));
}

TEST_CASE("parseXmf rejects non-XUMF / truncated data") {
  CHECK_FALSE(parseXmf("not a mesh at all").has_value());
  CHECK_FALSE(parseXmf(std::string(64, '\0')).has_value());
}

TEST_CASE("parseXmf reads the material table into submeshes") {
  const auto mesh = parseXmf(fixtureWithMaterial(0, 12));
  REQUIRE(mesh.has_value());
  REQUIRE(mesh->submeshes.size() == 1);
  CHECK(mesh->submeshes[0].firstIndex == 0u);
  CHECK(mesh->submeshes[0].indexCount == 12u);
  CHECK(mesh->submeshes[0].materialRef == "test.mat");
}

TEST_CASE("parseXmf drops a material table whose range exceeds the index buffer") {
  const auto mesh = parseXmf(fixtureWithMaterial(0, 99));
  REQUIRE(mesh.has_value());       // geometry still parses...
  CHECK(mesh->submeshes.empty());  // ...but the corrupt table degrades to the fallback
}

TEST_CASE("MaterialLibrary maps color_glow_map (X4's real emissive property) to emissiveMap") {
  // The real material_library.xml never uses emission_map/emissive_map — station
  // window glow ships as color_glow_map (attribute order type-then-name, as in
  // the game file).
  const char* xml = R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="generic">
    <material name="windows">
      <properties>
        <property type="BitMap" name="diffuse_map" value="assets\tex\win_diff" />
        <property type="BitMap" name="color_glow_map" value="assets\tex\win_glow" />
      </properties>
    </material>
  </collection>
</materiallibrary>)";
  const MaterialLibrary lib{std::string(xml)};
  const auto def = lib.getMaterial("generic.windows");
  REQUIRE(def.has_value());
  CHECK(def->emissiveMap == R"(assets\tex\win_glow)");
}

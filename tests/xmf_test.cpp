#include "x4sb/assetpipe/xmf.hpp"

#include "x4sb/assetpipe/gltf.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

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
}  // namespace

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

TEST_CASE("xmfVertexCount reads the vertex count from the descriptor only") {
  const auto n = xmfVertexCount(fixture());
  REQUIRE(n.has_value());
  CHECK(*n == 4u);  // the tetra fixture has 4 vertices
}

TEST_CASE("xmfVertexCount rejects non-XUMF data") {
  CHECK_FALSE(xmfVertexCount("not a mesh at all").has_value());
  CHECK_FALSE(xmfVertexCount(std::string(8, '\0')).has_value());
}

TEST_CASE("meshToGltf emits valid glTF 2.0 with matching counts and an embedded buffer") {
  const auto mesh = parseXmf(fixture());
  REQUIRE(mesh.has_value());

  const std::string gltf = meshToGltf(*mesh);
  REQUIRE_FALSE(gltf.empty());

  const nlohmann::json doc = nlohmann::json::parse(gltf);
  CHECK(doc["asset"]["version"] == "2.0");
  CHECK(doc["accessors"][0]["count"] == 4);  // POSITION
  CHECK(doc["accessors"][0]["type"] == "VEC3");
  CHECK(doc["accessors"][1]["count"] == 12);              // indices
  CHECK(doc["meshes"][0]["primitives"][0]["mode"] == 4);  // TRIANGLES

  const std::string uri = doc["buffers"][0]["uri"];
  CHECK(uri.rfind("data:application/octet-stream;base64,", 0) == 0);
}

TEST_CASE("meshToGltf de-indexed expands vertices and omits the index buffer") {
  const auto mesh = parseXmf(fixture());
  REQUIRE(mesh.has_value());

  const nlohmann::json doc = nlohmann::json::parse(meshToGltf(*mesh, /*deindex=*/true));
  REQUIRE(doc["accessors"].size() == 1);      // POSITION only — no index accessor
  CHECK(doc["accessors"][0]["count"] == 12);  // one vertex per index (the tetra has 12)
  CHECK(doc["bufferViews"].size() == 1);
  const auto& prim = doc["meshes"][0]["primitives"][0];
  CHECK_FALSE(prim.contains("indices"));  // draws via glDrawArrays, immune to the u16 cap
  CHECK(prim["mode"] == 4);
}

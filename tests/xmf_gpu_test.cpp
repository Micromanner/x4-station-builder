#include "xmf_model.hpp"

#include <array>
#include <cstdio>
#include <limits>

bool verifyXmfUpload() {
  x4sb::XmfMesh source;
  source.positions = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {-1, -2, -3}};
  source.normals = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0, -1}};
  source.texcoords = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  source.colors = {{128, 64, 32, 0}, {255, 128, 0, 77}, {10, 20, 30, 128}, {50, 60, 70, 255}};
  source.indices = {0, 1, 2, 2, 3, 0};
  source.submeshes = {{0, 3, "a"}, {3, 3, "b"}};
  const auto direct = x4sb::editor::modelFromXmf(source);
  if (!direct) return false;
  // Display space: Z reflected on positions and normals; UVs verbatim; colors
  // are the XMF RGBA verbatim (alpha = dirt amount); one mesh + material per range.
  bool ok = direct->meshCount == 2;
  for (int i = 0; i < direct->meshCount && ok; ++i) {
    const auto& mesh = direct->meshes[i];
    ok = mesh.vertexCount == 4 && mesh.triangleCount == 1 && direct->meshMaterial[i] == i;
    for (int vertex = 0; vertex < mesh.vertexCount && ok; ++vertex) {
      const auto v = static_cast<std::size_t>(vertex);
      const auto& p = source.positions[v];
      const auto& n = source.normals[v];
      ok = mesh.vertices[vertex * 3] == static_cast<float>(p.x) &&
           mesh.vertices[vertex * 3 + 1] == static_cast<float>(p.y) &&
           mesh.vertices[vertex * 3 + 2] == -static_cast<float>(p.z) &&
           mesh.normals[vertex * 3 + 2] == -static_cast<float>(n.z) &&
           mesh.texcoords[vertex * 2] == static_cast<float>(source.texcoords[v].x) &&
           mesh.colors[vertex * 4] == source.colors[v][0] &&
           mesh.colors[vertex * 4 + 2] == source.colors[v][2] &&
           mesh.colors[vertex * 4 + 3] == source.colors[v][3];
    }
    for (int index = 0; index < 3 && ok; ++index)
      ok = mesh.indices[index] == source.indices[static_cast<std::size_t>(i * 3 + index)];
  }
  UnloadModel(*direct);

  // UV map 2 rides the bone-weight slot without the planet tangent frame; UV map 1
  // only on request (sky domes).
  source.texcoords1 = {{0.75, 0.125}, {1, 0}, {1, 1}, {0, 1}};
  source.texcoords2 = {{0.25, 0.5}, {1, 0}, {1, 1}, {0, 1}};
  source.vertexData1 = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}};
  const auto hull = x4sb::editor::modelFromXmf(source);
  const auto withUv1 = x4sb::editor::prepareXmfModel(source, false, false, /*uv1=*/true);
  ok = ok && withUv1 && (*withUv1)[0].texcoords2.size() == 8 &&
       (*withUv1)[0].texcoords2[0] == 0.75F && (*withUv1)[0].texcoords2[1] == 0.125F;
  // Station parts: the part's vertices once, every stream present. A p2_multimat range
  // (USEEXTENDEDVDATA) reads a second block: DATA2 in the data slot, DATA1.w as COLOR0 alpha.
  source.vertexData2 = {{21, 22, 23, 24}, {25, 26, 27, 28}, {29, 30, 31, 32}, {33, 34, 35, 36}};
  const auto mixed = x4sb::editor::prepareStationPart(source, {false, true});
  ok = ok && mixed && mixed->vertices.size() == 8 && mixed->indices == source.indices &&
       mixed->hasNormals && mixed->ranges.size() == 2 && mixed->ranges[0].baseVertex == 0 &&
       mixed->ranges[1].baseVertex == 4 && mixed->ranges[1].firstIndex == 3 &&
       mixed->ranges[1].indexCount == 3;
  if (mixed) {
    const auto& plainVertex = mixed->vertices[0];
    const auto& extendedVertex = mixed->vertices[5];
    ok = ok && plainVertex.position[2] == -3.0F &&
         plainVertex.normal == x4sb::editor::packSnorm10(0, 0, -1) && plainVertex.uv1[0] == 0.75F &&
         plainVertex.uv23[0] == 0.25F && plainVertex.uv23[1] == 0.5F && plainVertex.data[2] == 3 &&
         plainVertex.color[3] == 0 && extendedVertex.data[2] == 27 &&
         extendedVertex.color[3] == 8 && extendedVertex.color[0] == 255;
  }
  const auto plain = x4sb::editor::prepareStationPart(source);
  ok = ok && plain && plain->vertices.size() == 4 && plain->ranges[1].baseVertex == 0;
  // INT_2_10_10_10_REV: x in bits 0-9, two's complement.
  ok = ok && x4sb::editor::packSnorm10(1, -1, 0) == (0x1FFU | (0x201U << 10U));
  source.vertexData2.clear();
  if (!hull) return false;
  const auto& hullMesh = hull->meshes[0];
  ok = ok && hullMesh.tangents == nullptr && hullMesh.boneWeights != nullptr &&
       hullMesh.boneWeights[0] == 0.25F && hullMesh.boneWeights[1] == 0.5F &&
       hullMesh.boneIds == nullptr && hullMesh.texcoords2 == nullptr;
  UnloadModel(*hull);
  source.texcoords1.clear();
  source.texcoords2.clear();
  source.vertexData1.clear();

  source.positions.resize(65537, {0, 0, 0});
  source.normals.clear();
  source.texcoords.clear();
  source.colors.clear();
  source.submeshes.clear();
  source.positions[65536] = {91, 92, 93};
  source.indices = {0, 65536, 2};
  const auto oversized = x4sb::editor::modelFromXmf(source);
  if (!oversized) return false;
  const auto& expanded = oversized->meshes[0];
  ok = ok && expanded.vertexCount == 3 && expanded.indices == nullptr &&
       expanded.vertices[3] == 91 && expanded.vertices[5] == -93 && expanded.colors[0] == 128 &&
       expanded.colors[3] == 255;
  UnloadModel(*oversized);
  // 32-bit indices: a station part keeps its index buffer past 65535 vertices.
  const auto bigPart = x4sb::editor::prepareStationPart(source);
  ok = ok && bigPart && bigPart->vertices.size() == 65537 && bigPart->indices[1] == 65536 &&
       !bigPart->hasNormals && bigPart->vertices[65536].position[2] == -93.0F &&
       bigPart->vertices[0].color[0] == 128 && bigPart->vertices[0].color[3] == 255 &&
       bigPart->boundsMin == std::array<float, 3>{-1, -2, -93} &&
       bigPart->boundsMax == std::array<float, 3>{91, 92, 3};
  const auto skyModel = x4sb::editor::modelFromXmf(source, true);
  if (!skyModel) return false;
  ok = ok && skyModel->meshes[0].colors[0] == 255;
  UnloadModel(*skyModel);

  source.indices[1] = 65537;
  auto bad = x4sb::editor::modelFromXmf(source);
  if (bad) {
    UnloadModel(*bad);
    ok = false;
  }
  source.indices[1] = 1;
  source.positions[0].x = std::numeric_limits<double>::infinity();
  bad = x4sb::editor::modelFromXmf(source);
  if (bad) {
    UnloadModel(*bad);
    ok = false;
  }
  source.positions[0].x = 0;
  source.submeshes = {{1, 3, "invalid"}};
  bad = x4sb::editor::modelFromXmf(source);
  if (bad) {
    UnloadModel(*bad);
    ok = false;
  }
  if (!ok) std::fprintf(stderr, "Direct XMF upload/validation failed\n");
  return ok;
}

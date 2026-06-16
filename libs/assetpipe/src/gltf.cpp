#include "x4sb/assetpipe/gltf.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>

namespace x4sb {
namespace {

using nlohmann::json;

std::string base64(const std::string& in) {
  static const char* const kTbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  auto byte = [&](std::size_t i) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(in[i]));
  };
  auto emit = [&](std::uint32_t v) { out.push_back(kTbl[static_cast<std::size_t>(v & 0x3FU)]); };

  std::size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    const std::uint32_t n = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
    emit(n >> 18);
    emit(n >> 12);
    emit(n >> 6);
    emit(n);
  }
  const std::size_t rem = in.size() - i;
  if (rem == 1) {
    const std::uint32_t n = byte(i) << 16;
    emit(n >> 18);
    emit(n >> 12);
    out += "==";
  } else if (rem == 2) {
    const std::uint32_t n = (byte(i) << 16) | (byte(i + 1) << 8);
    emit(n >> 18);
    emit(n >> 12);
    emit(n >> 6);
    out += '=';
  }
  return out;
}

void appendBytes(std::string& blob, const void* p, std::size_t n) {
  blob.append(static_cast<const char*>(p), n);
}

}  // namespace

std::string meshToGltf(const XmfMesh& mesh, bool deindex) {
  if (mesh.positions.empty() || mesh.indices.empty()) return {};

  // De-indexing dereferences positions[idx]; validate every index up front so a
  // corrupt/hostile .xmf can't drive an out-of-bounds read (untrusted-input bar).
  if (deindex) {
    for (const std::uint32_t idx : mesh.indices)
      if (idx >= mesh.positions.size()) return {};
  }

  std::string blob;
  std::array<float, 3> lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::max()};
  std::array<float, 3> hi{std::numeric_limits<float>::lowest(),
                          std::numeric_limits<float>::lowest(),
                          std::numeric_limits<float>::lowest()};
  const auto appendVertex = [&](const Vec3& v) {
    const std::array<float, 3> f{static_cast<float>(v.x), static_cast<float>(v.y),
                                 static_cast<float>(v.z)};
    for (std::size_t k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], f[k]);
      hi[k] = std::max(hi[k], f[k]);
    }
    appendBytes(blob, f.data(), f.size() * sizeof(float));
  };

  // Vertex stream: the raw positions (indexed), or one expanded vertex per index
  // (de-indexed — no index buffer follows, so the primitive draws via glDrawArrays).
  if (deindex) {
    blob.reserve(mesh.indices.size() * 12);
    for (const std::uint32_t idx : mesh.indices) appendVertex(mesh.positions[idx]);
  } else {
    blob.reserve(mesh.positions.size() * 12 + mesh.indices.size() * 4);
    for (const Vec3& v : mesh.positions) appendVertex(v);
  }
  const std::size_t posBytes = blob.size();
  const std::size_t vertexCount = deindex ? mesh.indices.size() : mesh.positions.size();

  json doc;
  doc["asset"] = {{"version", "2.0"}, {"generator", "x4sb-pipeline"}};
  doc["accessors"] = json::array({{{"bufferView", 0},
                                   {"componentType", 5126},
                                   {"count", vertexCount},
                                   {"type", "VEC3"},
                                   {"min", {lo[0], lo[1], lo[2]}},
                                   {"max", {hi[0], hi[1], hi[2]}}}});
  json bufferViews = json::array(
      {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", posBytes}, {"target", 34962}}});
  json primitive = {{"attributes", {{"POSITION", 0}}}, {"mode", 4}};

  if (!deindex) {
    for (const std::uint32_t idx : mesh.indices) appendBytes(blob, &idx, sizeof(idx));
    const std::size_t idxBytes = blob.size() - posBytes;
    bufferViews.push_back(
        {{"buffer", 0}, {"byteOffset", posBytes}, {"byteLength", idxBytes}, {"target", 34963}});
    doc["accessors"].push_back(
        {{"bufferView", 1}, {"componentType", 5125}, {"count", mesh.indices.size()}, {"type", "SCALAR"}});
    primitive["indices"] = 1;
  }

  doc["buffers"] = json::array({{{"byteLength", blob.size()},
                                 {"uri", "data:application/octet-stream;base64," + base64(blob)}}});
  doc["bufferViews"] = std::move(bufferViews);
  doc["meshes"] = json::array({{{"primitives", json::array({std::move(primitive)})}}});
  doc["nodes"] = json::array({{{"mesh", 0}}});
  doc["scenes"] = json::array({{{"nodes", json::array({0})}}});
  doc["scene"] = 0;
  return doc.dump(2);
}

bool writeGltfFile(const XmfMesh& mesh, const std::string& path, bool deindex) {
  const std::string text = meshToGltf(mesh, deindex);
  if (text.empty()) return false;
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

}  // namespace x4sb

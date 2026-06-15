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

std::string meshToGltf(const XmfMesh& mesh) {
  if (mesh.positions.empty() || mesh.indices.empty()) return {};

  // Binary buffer: float32 positions, then uint32 indices (both 4-byte aligned).
  std::string blob;
  blob.reserve(mesh.positions.size() * 12 + mesh.indices.size() * 4);

  std::array<float, 3> lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::max()};
  std::array<float, 3> hi{std::numeric_limits<float>::lowest(),
                          std::numeric_limits<float>::lowest(),
                          std::numeric_limits<float>::lowest()};
  for (const Vec3& v : mesh.positions) {
    const std::array<float, 3> f{static_cast<float>(v.x), static_cast<float>(v.y),
                                 static_cast<float>(v.z)};
    for (std::size_t k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], f[k]);
      hi[k] = std::max(hi[k], f[k]);
    }
    appendBytes(blob, f.data(), f.size() * sizeof(float));
  }
  const std::size_t posBytes = blob.size();
  for (const std::uint32_t idx : mesh.indices) appendBytes(blob, &idx, sizeof(idx));
  const std::size_t idxBytes = blob.size() - posBytes;

  const std::size_t vertexCount = mesh.positions.size();
  const std::size_t indexCount = mesh.indices.size();

  json doc;
  doc["asset"] = {{"version", "2.0"}, {"generator", "x4sb-pipeline"}};
  doc["buffers"] = json::array({{{"byteLength", blob.size()},
                                 {"uri", "data:application/octet-stream;base64," + base64(blob)}}});
  doc["bufferViews"] = json::array({
      {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", posBytes}, {"target", 34962}},
      {{"buffer", 0}, {"byteOffset", posBytes}, {"byteLength", idxBytes}, {"target", 34963}},
  });
  doc["accessors"] = json::array({
      {{"bufferView", 0},
       {"componentType", 5126},
       {"count", vertexCount},
       {"type", "VEC3"},
       {"min", {lo[0], lo[1], lo[2]}},
       {"max", {hi[0], hi[1], hi[2]}}},
      {{"bufferView", 1}, {"componentType", 5125}, {"count", indexCount}, {"type", "SCALAR"}},
  });
  doc["meshes"] = json::array(
      {{{"primitives",
         json::array({{{"attributes", {{"POSITION", 0}}}, {"indices", 1}, {"mode", 4}}})}}});
  doc["nodes"] = json::array({{{"mesh", 0}}});
  doc["scenes"] = json::array({{{"nodes", json::array({0})}}});
  doc["scene"] = 0;
  return doc.dump(2);
}

bool writeGltfFile(const XmfMesh& mesh, const std::string& path) {
  const std::string text = meshToGltf(mesh);
  if (text.empty()) return false;
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

}  // namespace x4sb

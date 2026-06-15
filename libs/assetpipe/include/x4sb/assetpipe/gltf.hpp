#pragma once
// Minimal glTF 2.0 writer for converted meshes (spec §4A). Emits a single,
// self-contained .gltf (POSITION + indices, binary buffer embedded as a base64
// data URI) that raylib / any glTF loader can open. Flat meshes are enough for
// v1 (spec §3); normals/materials are later polish. Render-free.
#include "x4sb/assetpipe/xmf.hpp"

#include <string>

namespace x4sb {

// Serialize a mesh to glTF 2.0 JSON text. Returns empty string if the mesh has
// no geometry.
std::string meshToGltf(const XmfMesh& mesh);

// Write meshToGltf() to a file. Returns false on empty mesh or IO error.
bool writeGltfFile(const XmfMesh& mesh, const std::string& path);

}  // namespace x4sb

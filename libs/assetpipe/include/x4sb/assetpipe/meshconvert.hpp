#pragma once
// Batch glTF mesh converter (spec §4A): for every buildable station module, read
// its source .xmf part meshes and write each to EXACTLY the path the catalog
// records (meshGltfPath). Reuses the catalog builder's module walk so output
// paths and catalog.json's meshRefs[].gltfPath cannot drift. Reads only through
// the `extract` callback; writes glTF files to disk. Render-free.
#include "x4sb/assetpipe/catalogbuild.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace x4sb {

struct MeshConvertResult {
  std::size_t modules{};    // modules visited
  std::size_t converted{};  // parts written this run
  std::size_t skipped{};    // parts whose output already existed (force == false)
  std::size_t failed{};     // parts whose source was missing or failed to parse/write
};

// Convert every module's .xmf part meshes to glTF under `outDir`, writing each to
// `outDir / meshGltfPath(moduleId, partName)`. Creates `outDir/meshes/`. When
// `force` is false, parts whose output already exists are skipped; when true they
// are always (re)converted. If `log` is non-empty it receives a concise message
// per module / event.
[[nodiscard]] MeshConvertResult convertModuleMeshes(
    const ExtractFn& extract, const std::vector<std::string>& sources, const std::string& outDir,
    bool force, const std::function<void(const std::string&)>& log);

}  // namespace x4sb

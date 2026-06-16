#pragma once
// Batch glTF mesh converter (spec §4A): for every buildable station module, read
// its source .xmf part meshes and write each to EXACTLY the path the catalog
// records (meshGltfPath). Reuses the catalog builder's module walk so output
// paths and catalog.json's meshRefs[].gltfPath cannot drift. Reads only through
// the `extract` callback; writes glTF files to disk. Render-free.
#include "x4sb/assetpipe/catalogbuild.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace x4sb {

struct MeshConvertResult {
  std::size_t modules{};     // modules visited
  std::size_t converted{};   // parts written this run
  std::size_t skipped{};     // parts whose output already existed (force == false)
  std::size_t failed{};      // parts whose source was missing or failed to parse/write
  std::size_t reducedLod{};  // parts converted at a LOD below 0 to fit the u16 limit
};

// raylib stores mesh indices as 16-bit, so a converted mesh must stay at/under
// this many vertices or it loads with wrapped indices (garbage triangles). The
// editor's MeshCache enforces the same limit before drawing.
inline constexpr std::uint32_t kU16VertexLimit = 65535;

// Choose which LOD to convert given each level's vertex count (nullopt = that LOD
// is not shipped). Prefers the most-detailed LOD (lowest index) whose count fits
// `budget`; if none fit, the available LOD with the fewest vertices; nullopt if no
// LOD is available at all. Keeps the heaviest meshes under the u16 index limit.
[[nodiscard]] std::optional<int> chooseMeshLod(
    const std::array<std::optional<std::uint32_t>, 4>& lodVertexCounts, std::uint32_t budget);

// Convert every module's .xmf part meshes to glTF under `outDir`, writing each to
// `outDir / meshGltfPath(moduleId, partName)`. Creates `outDir/meshes/`. When
// `force` is false, parts whose output already exists are skipped; when true they
// are always (re)converted. If `log` is non-empty it receives a concise message
// per module / event.
[[nodiscard]] MeshConvertResult convertModuleMeshes(
    const ExtractFn& extract, const std::vector<std::string>& sources, const std::string& outDir,
    bool force, const std::function<void(const std::string&)>& log);

}  // namespace x4sb

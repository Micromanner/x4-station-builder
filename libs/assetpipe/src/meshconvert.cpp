#include "x4sb/assetpipe/meshconvert.hpp"

#include "modulewalk.hpp"

#include "x4sb/assetpipe/component.hpp"
#include "x4sb/assetpipe/gltf.hpp"
#include "x4sb/assetpipe/xmf.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace x4sb {

namespace fs = std::filesystem;

std::optional<int> chooseMeshLod(const std::array<std::optional<std::uint32_t>, 4>& counts,
                                 std::uint32_t budget) {
  // Most-detailed (lowest index) LOD whose vertex count fits the budget.
  for (std::size_t i = 0; i < counts.size(); ++i)
    if (counts[i] && *counts[i] <= budget) return static_cast<int>(i);
  // None fit: fall back to the available LOD with the fewest vertices.
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < counts.size(); ++i) {
    if (!counts[i]) continue;
    if (!best || *counts[i] < *counts[*best]) best = i;
  }
  if (best) return static_cast<int>(*best);
  return std::nullopt;
}

MeshConvertResult convertModuleMeshes(const ExtractFn& extract,
                                      const std::vector<std::string>& sources,
                                      const std::string& outDir, bool force,
                                      const std::function<void(const std::string&)>& log) {
  MeshConvertResult res;
  const auto note = [&log](const std::string& msg) {
    if (log) log(msg);
  };

  // Create the output mesh dir up front; report the root cause once if it can't
  // be made (otherwise every writeGltfFile fails and is counted with no reason).
  const fs::path meshDir = fs::path(outDir) / "meshes";
  std::error_code mkdirEc;
  fs::create_directories(meshDir, mkdirEc);
  if (mkdirEc && !fs::is_directory(meshDir)) {
    note("[fail] could not create " + meshDir.string() + ": " + mkdirEc.message());
  }

  std::vector<std::string> skipped;  // resolution skips; surfaced via log, not tallied as failed
  detail::forEachResolvedModule(
      extract, sources,
      [&](const detail::ResolvedModule& rm) {
        ++res.modules;
        const ComponentGeometry geo = parseComponentGeometry(rm.componentXml);
        for (const ComponentPart& part : geo.parts) {
          // The cache-relative path; also the log key so a line maps to one file.
          const std::string rel = meshGltfPath(rm.id, part.name);
          const fs::path outPath = fs::path(outDir) / rel;

          // Fresh error_code per probe: a shared one could carry a stale failure,
          // and an IO error on the probe must NOT be read as "absent" (which would
          // silently proceed to overwrite/convert and mask the error).
          std::error_code existsEc;
          if (!force && fs::exists(outPath, existsEc) && !existsEc) {
            ++res.skipped;
            note("[skip] " + rel + " (exists)");
            continue;
          }

          // An xref part's mesh lives in another component's folder (pier/dock
          // modules instance shared sub-assemblies); resolve it, else use the
          // module's own geometry folder and the part's local name.
          std::string srcFolder = geo.geometryFolder;
          std::string srcName = part.name;
          if (const std::optional<PartRef> pr = parsePartRef(part.ref)) {
            const std::optional<std::string> f =
                rm.resolveComponentFolder ? rm.resolveComponentFolder(pr->component) : std::nullopt;
            if (f) {
              srcFolder = *f;
              srcName = pr->part;
            }
          }

          // Pick the most-detailed LOD that fits raylib's u16 index limit. lod0 is
          // the common case (most parts have no lower LOD and already fit), so only
          // probe lod1-3 when lod0 is missing or too dense — keeps the batch fast.
          std::array<std::optional<std::string>, 4> lodBytes;
          std::array<std::optional<std::uint32_t>, 4> lodCounts;
          lodBytes[0] = extract(partXmfPathLod(srcFolder, srcName, 0));
          if (lodBytes[0]) lodCounts[0] = xmfVertexCount(*lodBytes[0]);

          int chosen = 0;
          const bool lod0Fits = lodCounts[0] && *lodCounts[0] <= kU16VertexLimit;
          if (!lod0Fits) {
            for (int lod = 1; lod < 4; ++lod) {
              const auto idx = static_cast<std::size_t>(lod);
              lodBytes[idx] = extract(partXmfPathLod(srcFolder, srcName, lod));
              if (lodBytes[idx]) lodCounts[idx] = xmfVertexCount(*lodBytes[idx]);
            }
            const std::optional<int> pick = chooseMeshLod(lodCounts, kU16VertexLimit);
            if (!pick) {
              ++res.failed;
              note("[fail] " + rel + " (no LOD source)");
              continue;
            }
            chosen = *pick;
          }

          const std::optional<XmfMesh> mesh = parseXmf(*lodBytes[static_cast<std::size_t>(chosen)]);
          if (!mesh) {
            ++res.failed;
            note("[fail] " + rel + " (xmf parse failed)");
            continue;
          }
          // No LOD fit the u16 limit (e.g. piers ship identical LODs): de-index so
          // the mesh draws via glDrawArrays instead of boxing the whole module.
          const bool deindex = mesh->positions.size() > kU16VertexLimit;
          if (!writeGltfFile(*mesh, outPath.string(), deindex)) {
            ++res.failed;
            note("[fail] " + rel + " (gltf write failed)");
            continue;
          }
          ++res.converted;
          if (deindex) ++res.deindexed;
          if (chosen > 0) ++res.reducedLod;
          note("[ok]   " + rel + (deindex ? " (de-indexed)" : chosen > 0 ? " (lod" + std::to_string(chosen) + ")" : ""));
        }
      },
      skipped);

  for (const std::string& s : skipped) note("[skip-module] " + s);
  return res;
}

}  // namespace x4sb

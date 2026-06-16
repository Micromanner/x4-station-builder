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

          const std::string logical = partXmfPath(geo.geometryFolder, part.name);
          const std::optional<std::string> meshBytes = extract(logical);
          if (!meshBytes) {
            ++res.failed;
            note("[fail] " + rel + " (no " + logical + ")");
            continue;
          }
          const std::optional<XmfMesh> mesh = parseXmf(*meshBytes);
          if (!mesh) {
            ++res.failed;
            note("[fail] " + rel + " (xmf parse failed)");
            continue;
          }
          if (!writeGltfFile(*mesh, outPath.string())) {
            ++res.failed;
            note("[fail] " + rel + " (gltf write failed)");
            continue;
          }
          ++res.converted;
          note("[ok]   " + rel);
        }
      },
      skipped);

  for (const std::string& s : skipped) note("[skip-module] " + s);
  return res;
}

}  // namespace x4sb

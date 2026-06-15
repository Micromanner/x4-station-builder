// x4sb-pipeline — offline asset pipeline CLI (spec §4A).
//
// Reads an X4 install with the in-house .cat/.dat reader (no XRCatTool), parses a
// station module's component XML for its snap connectors and part meshes, and
// converts each .xmf part mesh to glTF in the output cache. This is the thin CLI
// wrapper over the shared libs/assetpipe logic; the editor drives the same code
// behind its first-run wizard.
#include "x4sb/archive/archive.hpp"
#include "x4sb/assetpipe/catalogbuild.hpp"
#include "x4sb/assetpipe/component.hpp"
#include "x4sb/assetpipe/gltf.hpp"
#include "x4sb/assetpipe/xmf.hpp"
#include "x4sb/data/catalog.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace x4sb;

constexpr const char* kDefaultComponent =
    "assets/structures/connectionmodules/struct_arg_cross_01.xml";
constexpr const char* kDefaultOut = "asset-cache";

void printUsage(const char* exe) {
  std::cout << "x4sb-pipeline — X4 asset pipeline (offline)\n\n"
            << "Usage:\n  " << exe << " --x4 <X4_install_path> [--out <dir>]"
            << " [--component <logical/path.xml>]\n\n"
            << "Converts a station module's .xmf part meshes to glTF and reports its\n"
            << "snap connectors.\n"
            << "  --out        output asset cache (default: " << kDefaultOut << ")\n"
            << "  --component  module component XML (default: " << kDefaultComponent << ")\n"
            << "  --catalog    build the full module catalog.json (ignores --component)\n";
}

// Replace path separators so a logical path becomes a flat output filename.
std::string flatten(const std::string& s) {
  std::string out = s;
  for (char& c : out)
    if (c == '/' || c == '\\') c = '_';
  return out;
}

int run(const std::string& x4Path, const std::string& outDir, const std::string& componentPath) {
  Archive archive;
  const int cats = archive.addInstall(x4Path);
  if (cats == 0) {
    std::cerr << "error: no catalogs found under " << x4Path << "\n";
    return 1;
  }
  std::cout << "indexed " << archive.fileCount() << " files from " << cats << " catalogs\n";

  const auto componentXml = archive.extract(componentPath);
  if (!componentXml) {
    std::cerr << "error: component not found: " << componentPath << "\n";
    return 1;
  }

  const auto connectors = snapConnectionPoints(*componentXml);
  std::cout << "snap connectors: " << connectors.size() << "\n";
  for (const auto& c : connectors)
    std::cout << "  " << c.id << "  pos(" << c.localPosition.x << ", " << c.localPosition.y << ", "
              << c.localPosition.z << ")\n";

  const ComponentGeometry geo = parseComponentGeometry(*componentXml);
  std::cout << "geometry folder: " << geo.geometryFolder << "\n"
            << "part meshes: " << geo.parts.size() << "\n";

  std::error_code ec;
  fs::create_directories(outDir, ec);

  std::size_t converted = 0, totalVerts = 0, totalTris = 0;
  bool haveBounds = false;
  AABB bounds{};
  XmfMesh assembled;  // all parts merged at their connection offsets (whole module)
  for (const auto& part : geo.parts) {
    const std::string logical = geo.geometryFolder + "/" + part.name + "-lod0.xmf";
    const auto meshBytes = archive.extract(logical);
    if (!meshBytes) {
      std::cout << "  [skip] " << part.name << " (no " << logical << ")\n";
      continue;
    }
    const auto mesh = parseXmf(*meshBytes);
    if (!mesh) {
      std::cout << "  [fail] " << part.name << " (xmf parse failed)\n";
      continue;
    }
    const std::string outPath =
        (fs::path(outDir) / (flatten(componentPath) + "__" + part.name + ".gltf")).string();
    if (!writeGltfFile(*mesh, outPath)) {
      std::cout << "  [fail] " << part.name << " (gltf write failed)\n";
      continue;
    }
    ++converted;
    totalVerts += mesh->positions.size();
    totalTris += mesh->indices.size() / 3;
    bounds = haveBounds ? merge(bounds, mesh->bounds) : mesh->bounds;
    haveBounds = true;
    // Merge this part into the whole-module mesh at its mount offset.
    const std::size_t base = assembled.positions.size();
    for (const Vec3& v : mesh->positions) assembled.positions.push_back(apply(part.offset, v));
    for (const std::uint32_t i : mesh->indices)
      assembled.indices.push_back(static_cast<std::uint32_t>(base) + i);

    std::cout << "  [ok]   " << part.name << ": " << mesh->positions.size() << " verts, "
              << mesh->indices.size() / 3 << " tris  -> " << fs::path(outPath).filename().string()
              << "\n";
  }

  if (!assembled.positions.empty()) {
    const std::string asmPath =
        (fs::path(outDir) / (flatten(componentPath) + "__assembled.gltf")).string();
    if (writeGltfFile(assembled, asmPath))
      std::cout << "assembled module: " << assembled.positions.size() << " verts, "
                << assembled.indices.size() / 3 << " tris -> " << fs::path(asmPath).filename().string()
                << "\n";
  }

  std::cout << "\nconverted " << converted << "/" << geo.parts.size() << " parts, " << totalVerts
            << " verts, " << totalTris << " tris\n";
  if (haveBounds)
    std::cout << "module AABB: x[" << bounds.min.x << ", " << bounds.max.x << "] y[" << bounds.min.y
              << ", " << bounds.max.y << "] z[" << bounds.min.z << ", " << bounds.max.z << "]\n";
  return 0;
}

int runCatalog(const std::string& x4Path, const std::string& outDir) {
  Archive archive;
  const int cats = archive.addInstall(x4Path);
  if (cats == 0) {
    std::cerr << "error: no catalogs found under " << x4Path << "\n";
    return 1;
  }
  std::cout << "indexed " << archive.fileCount() << " files from " << cats << " catalogs\n";

  const ExtractFn extract = [&archive](const std::string& path) { return archive.extract(path); };
  const CatalogBuildResult res = buildModuleCatalog(extract, archive.sources());
  if (res.modules.empty()) {
    std::cerr << "error: no modules resolved (is libraries/wares.xml present?)\n";
    return 1;
  }

  std::error_code ec;
  fs::create_directories(outDir, ec);
  const std::string outPath = (fs::path(outDir) / "catalog.json").string();
  if (!writeCatalogFile(res.modules, outPath)) {
    std::cerr << "error: failed to write " << outPath << "\n";
    return 1;
  }

  std::cout << "wrote " << res.modules.size() << " modules -> " << outPath << "\n";
  if (!res.skipped.empty()) {
    std::cout << "skipped " << res.skipped.size() << " modules:\n";
    for (const std::string& s : res.skipped) std::cout << "  " << s << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string x4Path, outDir = kDefaultOut, componentPath = kDefaultComponent;
  bool catalog = false;

  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--x4" && i + 1 < args.size()) {
      x4Path = args[++i];
    } else if (args[i] == "--out" && i + 1 < args.size()) {
      outDir = args[++i];
    } else if (args[i] == "--component" && i + 1 < args.size()) {
      componentPath = args[++i];
    } else if (args[i] == "--catalog") {
      catalog = true;
    } else if (args[i] == "-h" || args[i] == "--help") {
      printUsage(argv[0]);
      return 0;
    }
  }

  if (x4Path.empty()) {  // only --x4 is required; --out defaults to the asset cache
    printUsage(argv[0]);
    return args.empty() ? 0 : 2;
  }
  if (catalog) return runCatalog(x4Path, outDir);
  return run(x4Path, outDir, componentPath);
}

#include "loadbench.hpp"

#include "app_paths.hpp"
#include "mesh_cache.hpp"
#include "mesh_load.hpp"
#include "render.hpp"

#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/mesh_paths.hpp"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace x4sb::editor {
namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;
constexpr double kSpacing = 1500.0;  // ~13 modules/axis at 2000 modules => ~20km cube (max plot)

std::vector<std::string> meshfulMacros(const ModuleCatalog& catalog) {
  std::vector<std::string> ids;
  for (const auto& [id, def] : catalog.all())
    if (!def.meshRefs.empty()) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  return ids;
}

// `modules` total, cycling through the distinct macros (repeats) so the station has
// realistic mesh variety AND a realistic module count.
Station makeStation(const std::vector<std::string>& macros, int n) {
  Station s;
  const int side = static_cast<int>(std::ceil(std::cbrt(static_cast<double>(n))));
  int placed = 0;
  for (int ix = 0; ix < side && placed < n; ++ix)
    for (int iy = 0; iy < side && placed < n; ++iy)
      for (int iz = 0; iz < side && placed < n; ++iz, ++placed) {
        PlacedModule pm;
        pm.defId = macros[static_cast<std::size_t>(placed) % macros.size()];
        pm.worldTransform.position = {static_cast<double>(ix - side / 2) * kSpacing,
                                      static_cast<double>(iy - side / 2) * kSpacing,
                                      static_cast<double>(iz - side / 2) * kSpacing};
        s.add(pm);
      }
  return s;
}

double meanFps(const Station& station, const ModuleCatalog& catalog, const ::Camera3D& cam,
               MeshCache& meshes, bool lodEnabled, int frames) {
  for (int i = 0; i < 3; ++i) {  // warm-up
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    drawScene(station, catalog, cam, meshes, false, true, false, lodEnabled);
    EndDrawing();
  }
  double sum = 0.0;
  int n = 0;
  for (int i = 0; i < frames && !WindowShouldClose(); ++i) {
    const double t0 = GetTime();
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    drawScene(station, catalog, cam, meshes, false, true, false, lodEnabled);
    EndDrawing();
    sum += (GetTime() - t0) * 1000.0;
    ++n;
  }
  const double meanMs = n > 0 ? sum / static_cast<double>(n) : 0.0;
  return meanMs > 0.0 ? 1000.0 / meanMs : 0.0;
}

}  // namespace

int runLoadBench(int modules) {
  const std::optional<std::string> catalogPath = findCatalogJson();
  if (!catalogPath) {
    std::fprintf(stderr, "loadbench: could not find asset-cache/catalog.json\n");
    return 1;
  }
  std::optional<ModuleCatalog> catalog = ModuleCatalog::loadFromFile(*catalogPath);
  if (!catalog) {
    std::fprintf(stderr, "loadbench: failed to load catalog\n");
    return 1;
  }
  const std::vector<std::string> macros = meshfulMacros(*catalog);
  if (macros.empty()) {
    std::fprintf(stderr, "loadbench: no modules with meshes in catalog\n");
    return 1;
  }

  const int n = std::max(modules, 1);
  const Station station = makeStation(macros, n);
  const std::vector<std::string> paths = meshPathsFor(station, *catalog);
  const StationBounds bounds = stationBounds(station, *catalog);
  std::printf("loadbench: %zu modules (%zu macros), %zu unique meshes, radius=%.0f\n",
              station.modules().size(), macros.size(), paths.size(), bounds.radius);

  InitWindow(kScreenW, kScreenH, "X4 Station Builder - loadbench");
  {
    MeshCache meshes{std::filesystem::path(*catalogPath).parent_path()};
    const double t0 = GetTime();
    loadStationMeshes(station, *catalog, meshes);
    const double loadMs = (GetTime() - t0) * 1000.0;

    int resident = 0;
    int absent = 0;
    for (const std::string& p : paths) (meshes.get(p) != nullptr) ? ++resident : ++absent;
    std::printf("loadbench: pre-warm %.0f ms -> resident=%d missing/oversized=%d (of %zu)\n", loadMs,
                resident, absent, paths.size());

    const Vec3 dt = flipZ(bounds.center);
    const double dist = std::max(bounds.radius, 1.0) * 1.6;
    const ::Camera3D cam{
        ::Vector3{static_cast<float>(dt.x + dist * 0.6), static_cast<float>(dt.y + dist * 0.5),
                  static_cast<float>(dt.z + dist * 0.6)},
        ::Vector3{static_cast<float>(dt.x), static_cast<float>(dt.y), static_cast<float>(dt.z)},
        ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};

    const RenderStats st = lodStats(station, *catalog, cam);
    std::printf("loadbench: view = %d drawn (%d detailed / %d boxed at LOD), %d culled\n",
                st.drawnDetailed + st.drawnBox, st.drawnDetailed, st.drawnBox, st.culled);

    const double fpsLodOn = meanFps(station, *catalog, cam, meshes, /*lodEnabled=*/true, 40);
    const double fpsLodOff = meanFps(station, *catalog, cam, meshes, /*lodEnabled=*/false, 40);
    std::printf("loadbench: FPS  LOD-on=%.0f  LOD-off(all meshes)=%.0f\n", fpsLodOn, fpsLodOff);
  }
  CloseWindow();
  return 0;
}

}  // namespace x4sb::editor

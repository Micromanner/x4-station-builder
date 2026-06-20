#include "connbench.hpp"

#include "app_paths.hpp"
#include "mesh_cache.hpp"
#include "raylib.h"
#include "render.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/editor_state.hpp"

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
constexpr int kWarmup = 8;  // let the budgeted mesh uploads finish before timing

// Pick the module with the most connectors among those that have meshes — the
// connector pass is what we're stressing, the meshes keep the base render realistic.
const ModuleDef* pickModule(const ModuleCatalog& catalog) {
  const ModuleDef* best = nullptr;
  for (const auto& [id, def] : catalog.all()) {
    if (def.meshRefs.empty() || def.connectionPoints.empty()) continue;
    if (best == nullptr || def.connectionPoints.size() > best->connectionPoints.size()) best = &def;
  }
  return best;
}

double extentMax(const AABB& a) {
  const Vec3 e = a.max - a.min;
  return std::max({e.x, e.y, e.z});
}

// N copies of `def` on a cube grid, spaced so the station spans a large volume
// (like a real big station) — so the grid cull has distant connectors to skip.
Station makeStation(const ModuleDef& def, int n, double spacing) {
  const int side = static_cast<int>(std::ceil(std::cbrt(static_cast<double>(n))));
  Station s;
  int placed = 0;
  for (int ix = 0; ix < side && placed < n; ++ix)
    for (int iy = 0; iy < side && placed < n; ++iy)
      for (int iz = 0; iz < side && placed < n; ++iz, ++placed) {
        PlacedModule pm;
        pm.defId = def.id;
        pm.worldTransform.position = {(ix - side / 2) * spacing, (iy - side / 2) * spacing,
                                      (iz - side / 2) * spacing};
        s.add(pm);
      }
  return s;
}

double percentile(std::vector<double> ms, double p) {
  if (ms.empty()) return 0.0;
  std::sort(ms.begin(), ms.end());
  const double rank = p * static_cast<double>(ms.size() - 1);
  const std::size_t idx = static_cast<std::size_t>(std::lround(rank));
  return ms[std::min(idx, ms.size() - 1)];
}

struct Result {
  double meanMs{0};
  double p95Ms{0};
};

template <typename DrawFn>
Result timed(int frames, const DrawFn& draw) {
  for (int i = 0; i < kWarmup && !WindowShouldClose(); ++i) {
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    draw();
    EndDrawing();
  }
  std::vector<double> ms;
  ms.reserve(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames && !WindowShouldClose(); ++i) {
    const double t0 = GetTime();
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    draw();
    EndDrawing();
    ms.push_back((GetTime() - t0) * 1000.0);
  }
  double sum = 0.0;
  for (const double v : ms) sum += v;
  return {ms.empty() ? 0.0 : sum / static_cast<double>(ms.size()), percentile(ms, 0.95)};
}

void report(const char* label, const Result& r) {
  std::printf("  %-34s mean=%7.2f ms (%5.0f fps)   p95=%7.2f ms\n", label, r.meanMs,
              r.meanMs > 0 ? 1000.0 / r.meanMs : 0.0, r.p95Ms);
}

}  // namespace

int runConnBench(int modules, int frames, double spacingOverride) {
  const std::optional<std::string> catalogPath = findCatalogJson();
  if (!catalogPath) {
    std::fprintf(stderr, "connbench: could not find asset-cache/catalog.json\n");
    return 1;
  }
  std::optional<ModuleCatalog> catalog = ModuleCatalog::loadFromFile(*catalogPath);
  if (!catalog) {
    std::fprintf(stderr, "connbench: failed to load catalog\n");
    return 1;
  }
  const ModuleDef* def = pickModule(*catalog);
  if (def == nullptr) {
    std::fprintf(stderr, "connbench: no module with both meshes and connectors\n");
    return 1;
  }
  const double spacing =
      spacingOverride > 0.0 ? spacingOverride : std::max(extentMax(def->aabb) * 2.0, 1500.0);
  const int n = std::max(modules, 1);
  const Station station = makeStation(*def, n, spacing);
  const std::size_t conns = station.modules().size() * def->connectionPoints.size();

  // Selection at the central module for the grid-culled path: a downward ray
  // through its centre hits only it (neighbours are `spacing` away in X/Z).
  EditorState state(*catalog);
  state.loadStation(makeStation(*def, n, spacing));
  const auto& mods = state.station().modules();
  const Vec3 centre = mods[mods.size() / 2].worldTransform.position;
  state.selectByRay(centre + Vec3{0, spacing * 0.5, 0}, Vec3{0, -1, 0});

  const StationBounds bounds = stationBounds(station, *catalog);
  std::printf(
      "connbench: module=%s  modules=%d  connectors=%zu  spacing=%.0f  radius=%.0f  "
      "selected=%s  frames=%d\n",
      def->id.c_str(), n, conns, spacing, bounds.radius, state.selected() ? "yes" : "NO(!)",
      std::max(frames, 1));

  const Vec3 dt = flipZ(bounds.center);
  const ::Camera3D camera{
      ::Vector3{static_cast<float>(dt.x + bounds.radius * 1.4),
                static_cast<float>(dt.y + bounds.radius * 0.7),
                static_cast<float>(dt.z + bounds.radius * 1.4)},
      ::Vector3{static_cast<float>(dt.x), static_cast<float>(dt.y), static_cast<float>(dt.z)},
      ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};

  InitWindow(kScreenW, kScreenH, "X4 Station Builder - connbench");
  {
    MeshCache meshes{std::filesystem::path(*catalogPath).parent_path()};
    const int f = std::max(frames, 1);

    const Result baseline = timed(f, [&] {
      drawScene(station, *catalog, camera, meshes, /*showGizmos=*/false, /*showMeshes=*/true,
                /*allConnectors=*/false);
    });
    const Result oldAll = timed(f, [&] {
      drawScene(station, *catalog, camera, meshes, /*showGizmos=*/false, /*showMeshes=*/true,
                /*allConnectors=*/true);
    });
    const Result newGrid = timed(
        f, [&] { drawScene(state, camera, meshes, /*showGizmos=*/false, /*showMeshes=*/true); });

    report("baseline (no connectors)", baseline);
    report("OLD: all connectors (pre-fix)", oldAll);
    report("NEW: grid-culled + selection", newGrid);

    const double oldPass = oldAll.meanMs - baseline.meanMs;
    const double newPass = newGrid.meanMs - baseline.meanMs;
    std::printf("  connector-pass cost: OLD=%.2f ms  NEW=%.2f ms  -> %.1fx cheaper\n", oldPass,
                newPass, (newPass > 0.01) ? oldPass / newPass : 0.0);
  }
  CloseWindow();
  return 0;
}

}  // namespace x4sb::editor

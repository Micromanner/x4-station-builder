#include "profile.hpp"

#include "app_paths.hpp"
#include "mesh_cache.hpp"
#include "mesh_load.hpp"
#include "profiling.hpp"
#include "raylib.h"
#include "render.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/planio/plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace x4sb::editor {
namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;
constexpr int kWarmup = 5;  // exclude mesh-upload/shader-compile spikes from the stats

// Nearest-rank percentile of an already-sorted (ascending) sample, p in [0,1].
[[nodiscard]] double percentile(const std::vector<double>& sortedMs, double p) {
  if (sortedMs.empty()) return 0.0;
  const double rank = p * static_cast<double>(sortedMs.size() - 1);
  const std::size_t idx = static_cast<std::size_t>(std::lround(rank));
  return sortedMs[std::min(idx, sortedMs.size() - 1)];
}

}  // namespace

int runProfile(const std::string& planPath, int frames) {
  const std::optional<std::string> catalogPath = findCatalogJson();
  if (!catalogPath) {
    std::fprintf(stderr, "profile: could not find asset-cache/catalog.json\n");
    return 1;
  }
  std::optional<ModuleCatalog> catalog = ModuleCatalog::loadFromFile(*catalogPath);
  if (!catalog) {
    std::fprintf(stderr, "profile: failed to load catalog %s\n", catalogPath->c_str());
    return 1;
  }
  std::ifstream in(planPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "profile: cannot open plan %s\n", planPath.c_str());
    return 1;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::optional<Station> station = importPlanXml(ss.str());
  if (!station) {
    std::fprintf(stderr, "profile: failed to parse plan %s\n", planPath.c_str());
    return 1;
  }

  const int total = std::max(frames, 1);
  const StationBounds bounds = stationBounds(*station, *catalog);
  std::printf("profile: modules=%zu radius=%.0f frames=%d\n", station->modules().size(),
              bounds.radius, total);

  InitWindow(kScreenW, kScreenH, "X4 Station Builder - profile");
  // No SetTargetFPS: uncapped so per-frame timing reflects real render cost.
  {
    MeshCache meshes{std::filesystem::path(*catalogPath).parent_path()};
    // Pre-upload every mesh exactly as the live editor does on plan-open
    // (main.cpp loadStationMeshes). Without this, profile mode uploaded lazily
    // via the budgeted get() path mid-run, so first-orbit LoadModel spikes
    // polluted the steady-state numbers and made the profiler unrepresentative
    // of the warmed live editor (known-issues 1.3 / cull+lod tail spikes).
    loadStationMeshes(*station, *catalog, meshes);

    const Vec3 dt = flipZ(bounds.center);  // display-space target (scene flips Z)
    const ::Vector3 target{static_cast<float>(dt.x), static_cast<float>(dt.y),
                           static_cast<float>(dt.z)};
    const double radius = std::max(bounds.radius, 1.0);
    const double dist = radius * 1.6;  // mid-range: a healthy mix of detailed meshes + LOD boxes
    constexpr double kPitch = 0.45;    // ~26 deg elevation

    // Camera for frame i, orbiting the station twice over the run so each frame
    // re-exercises culling/LOD from a fresh angle (a representative interactive
    // workload, not one static view the GPU could trivially cache).
    const auto cameraAt = [&](int i) {
      const double yaw =
          (static_cast<double>(i) / static_cast<double>(total)) * 4.0 * static_cast<double>(PI);
      const Vec3 dir{std::cos(kPitch) * std::sin(yaw), std::sin(kPitch),
                     std::cos(kPitch) * std::cos(yaw)};
      return ::Camera3D{::Vector3{target.x + static_cast<float>(dir.x * dist),
                                  target.y + static_cast<float>(dir.y * dist),
                                  target.z + static_cast<float>(dir.z * dist)},
                        target, ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};
    };

    for (int i = 0; i < kWarmup && !WindowShouldClose(); ++i) {
      BeginDrawing();
      ClearBackground(::Color{30, 30, 38, 255});
      drawScene(*station, *catalog, cameraAt(0), meshes, /*showGizmos=*/true, /*showMeshes=*/true);
      EndDrawing();
    }

    std::vector<double> frameMs;
    frameMs.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < total && !WindowShouldClose(); ++i) {
      const ::Camera3D camera = cameraAt(i);
      const double t0 = GetTime();
      {
        ZoneScopedN("frame");
        BeginDrawing();
        ClearBackground(::Color{30, 30, 38, 255});
        drawScene(*station, *catalog, camera, meshes, /*showGizmos=*/true, /*showMeshes=*/true);
        EndDrawing();
      }
      frameMs.push_back((GetTime() - t0) * 1000.0);
      FrameMark;  // Tracy frame boundary (no-op without the tracy build)
    }

    if (!frameMs.empty()) {
      double sum = 0.0;
      for (const double ms : frameMs) sum += ms;
      const double mean = sum / static_cast<double>(frameMs.size());
      std::sort(frameMs.begin(), frameMs.end());
      std::printf(
          "profile: frames=%zu  mean=%.2fms (%.0f fps)  p50=%.2f  p95=%.2f  p99=%.2f  "
          "max=%.2fms\n",
          frameMs.size(), mean, mean > 0 ? 1000.0 / mean : 0.0, percentile(frameMs, 0.50),
          percentile(frameMs, 0.95), percentile(frameMs, 0.99), frameMs.back());
    }
  }
  CloseWindow();
  return 0;
}

}  // namespace x4sb::editor

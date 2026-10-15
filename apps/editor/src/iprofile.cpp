#include "iprofile.hpp"

#include "bloom.hpp"
#include "mesh_cache.hpp"
#include "mesh_load.hpp"
#include "native_assets.hpp"
#include "profiling.hpp"
#include "raylib.h"
#include "render.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/editor_state.hpp"
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
constexpr int kWarmup = 5;

[[nodiscard]] double percentile(const std::vector<double>& sortedMs, double p) {
  if (sortedMs.empty()) return 0.0;
  const double rank = p * static_cast<double>(sortedMs.size() - 1);
  const std::size_t idx = static_cast<std::size_t>(std::lround(rank));
  return sortedMs[std::min(idx, sortedMs.size() - 1)];
}

}  // namespace

int runInteractiveProfile(const NativeAssets& native, const std::string& planPath, int frames) {
  const ModuleCatalog* catalog = &native.catalog;
  std::ifstream in(planPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "iprofile: cannot open plan %s\n", planPath.c_str());
    return 1;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::optional<Station> station = importPlanXml(ss.str());
  if (!station) {
    std::fprintf(stderr, "iprofile: failed to parse plan\n");
    return 1;
  }

  EditorState state(*catalog);
  state.loadStation(std::move(*station));
  const int total = std::max(frames, 1);
  const StationBounds bounds = stationBounds(state.station(), *catalog);
  std::printf("iprofile: modules=%zu radius=%.0f frames=%d (ghost driven at centre)\n",
              state.station().modules().size(), bounds.radius, total);

  InitWindow(kScreenW, kScreenH, "X4 Station Builder - iprofile");
  {
    MeshCache meshes{native};
    FramePipeline pipeline;
    loadStationMeshes(state.station(), *catalog, meshes);  // warm, like the live editor

    const Vec3 dt = flipZ(bounds.center);  // display-space camera target (scene flips Z)
    const ::Vector3 target{static_cast<float>(dt.x), static_cast<float>(dt.y),
                           static_cast<float>(dt.z)};
    const double radius = std::max(bounds.radius, 1.0);
    constexpr double kPitch = 0.45;
    // Orbit + dolly (0.4R..3R), like --profile: the depth sweep drives mass LOD switches.
    const auto cameraAt = [&](int i) {
      const double t = static_cast<double>(i) / static_cast<double>(total);
      const double yaw = t * 4.0 * static_cast<double>(PI);
      const double dist = radius * (1.7 + 1.3 * std::cos(t * 2.0 * static_cast<double>(PI)));
      const Vec3 dir{std::cos(kPitch) * std::sin(yaw), std::sin(kPitch),
                     std::cos(kPitch) * std::cos(yaw)};
      return ::Camera3D{::Vector3{target.x + static_cast<float>(dir.x * dist),
                                  target.y + static_cast<float>(dir.y * dist),
                                  target.z + static_cast<float>(dir.z * dist)},
                        target, ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};
    };

    // Drive a ghost at the dense station centre in X4 space: a downward ray from above
    // it engages the snap path (or free-places at centre), so the interactive draws run.
    state.setPlaceDistance(radius);
    const auto driveGhost = [&] {
      state.updateGhost(bounds.center + Vec3{0, radius, 0}, Vec3{0, -1, 0});
    };

    for (int i = 0; i < kWarmup && !WindowShouldClose(); ++i) {
      driveGhost();
      BeginDrawing();
      pipeline.begin();
      ClearBackground(::Color{30, 30, 38, 255});
      drawScene(state, cameraAt(0), meshes, /*showGizmos=*/true, /*showMeshes=*/true,
                /*lodEnabled=*/false);
      pipeline.end();
      EndDrawing();
    }

    std::vector<double> frameMs;
    frameMs.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < total && !WindowShouldClose(); ++i) {
      const ::Camera3D camera = cameraAt(i);
      driveGhost();
      const double t0 = GetTime();
      {
        ZoneScopedN("frame");
        BeginDrawing();
        pipeline.begin();
        ClearBackground(::Color{30, 30, 38, 255});
        drawScene(state, camera, meshes, /*showGizmos=*/true, /*showMeshes=*/true,
                  /*lodEnabled=*/false);
        pipeline.end();
        EndDrawing();
      }
      frameMs.push_back((GetTime() - t0) * 1000.0);
      FrameMark;
    }

    if (!frameMs.empty()) {
      double sum = 0.0;
      for (const double ms : frameMs) sum += ms;
      const double mean = sum / static_cast<double>(frameMs.size());
      std::sort(frameMs.begin(), frameMs.end());
      std::printf(
          "iprofile: frames=%zu  mean=%.2fms (%.0f fps)  p50=%.2f  p95=%.2f  p99=%.2f  "
          "max=%.2fms\n",
          frameMs.size(), mean, mean > 0 ? 1000.0 / mean : 0.0, percentile(frameMs, 0.50),
          percentile(frameMs, 0.95), percentile(frameMs, 0.99), frameMs.back());
    }
  }
  CloseWindow();
  return 0;
}

}  // namespace x4sb::editor

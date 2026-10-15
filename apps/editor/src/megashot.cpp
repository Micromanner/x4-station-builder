#include "megashot.hpp"

#include "bloom.hpp"
#include "env_cubemap.hpp"
#include "mesh_cache.hpp"
#include "native_assets.hpp"
#include "raylib.h"
#include "render.hpp"
#include "sky.hpp"
#include "sun_shadow.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/planio/plan.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace x4sb::editor {
namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

// One labelled view to render + tally. `distFactor` multiplies the station's
// bounding radius: >1 frames it from outside, <1 puts the eye inside the volume
// (the regime that exposes the behind-cull / near-plane symptoms). `yawDeg`
// orbits the eye so a sweep confirms no view-dependent half drops out.
struct Shot {
  const char* label;
  double distFactor;
  double yawDeg;
};

// Render `frames` frames (SetTargetFPS must be off so it runs uncapped), capture
// the last, and return the mean render FPS — the perf signal for a view.
double renderAndCapture(const Station& station, const ModuleCatalog& catalog,
                        const ::Camera3D& camera, MeshCache& meshes, Sky& sky,
                        FramePipeline& pipeline, SunShadow& shadow, const std::string& outPath) {
  constexpr int kFrames = 40;
  for (int i = 0; i < 3; ++i) {  // warm-up (mesh upload, shader) excluded from timing
    BeginDrawing();
    shadow.render(station, catalog, meshes, sky.sun().dir, camera);
    pipeline.begin();
    ClearBackground(::Color{30, 30, 38, 255});
    BeginMode3D(camera);
    sky.draw(camera);
    EndMode3D();
    drawScene(station, catalog, camera, meshes, /*showGizmos=*/true, /*showMeshes=*/true,
              /*allConnectors=*/false, /*lodEnabled=*/false, sky.sun());
    pipeline.end();
    EndDrawing();
  }
  const double t0 = GetTime();
  for (int i = 0; i < kFrames; ++i) {
    BeginDrawing();
    shadow.render(station, catalog, meshes, sky.sun().dir, camera);
    pipeline.begin();
    ClearBackground(::Color{30, 30, 38, 255});
    BeginMode3D(camera);
    sky.draw(camera);
    EndMode3D();
    drawScene(station, catalog, camera, meshes, /*showGizmos=*/true, /*showMeshes=*/true,
              /*allConnectors=*/false, /*lodEnabled=*/false, sky.sun());
    pipeline.end();
    EndDrawing();
  }
  const double dt = GetTime() - t0;
  TakeScreenshot(outPath.c_str());  // raylib writes basename into the working dir
  return dt > 0 ? kFrames / dt : 0.0;
}

}  // namespace

int runMegaShot(const NativeAssets& native, const std::string& planPath,
                const std::string& outPrefix) {
  const ModuleCatalog* catalog = &native.catalog;
  std::ifstream in(planPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "megashot: cannot open plan %s\n", planPath.c_str());
    return 1;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::optional<Station> station = importPlanXml(ss.str());
  if (!station) {
    std::fprintf(stderr, "megashot: failed to parse plan %s\n", planPath.c_str());
    return 1;
  }

  const StationBounds bounds = stationBounds(*station, *catalog);
  const Vec3 c = bounds.center;
  const double radius = bounds.radius;
  std::printf("megashot: modules=%zu bbox min=(%.0f,%.0f,%.0f) max=(%.0f,%.0f,%.0f)\n",
              station->modules().size(), bounds.box.min.x, bounds.box.min.y, bounds.box.min.z,
              bounds.box.max.x, bounds.box.max.y, bounds.box.max.z);
  std::printf("megashot: center=(%.0f,%.0f,%.0f) radius=%.0f\n", c.x, c.y, c.z, radius);

  // Env overrides so a live-editor report (window size, picked sky) can be
  // reproduced through the same path: X4SB_SHOT_W/H, X4SB_SHOT_CLUSTER.
  const auto envInt = [](const char* name, int fallback) {
    const char* v = std::getenv(name);
    const int parsed = v ? std::atoi(v) : 0;
    return parsed >= 64 ? parsed : fallback;
  };
  const char* clusterEnv = std::getenv("X4SB_SHOT_CLUSTER");
  InitWindow(envInt("X4SB_SHOT_W", kScreenW), envInt("X4SB_SHOT_H", kScreenH),
             "X4 Station Builder - megashot");
  // No SetTargetFPS: leave the frame rate uncapped so renderAndCapture's timing
  // reflects real render cost, not a 60 Hz cap.
  {
    MeshCache meshes{native};
    Sky sky{native, clusterEnv ? std::string(clusterEnv) : std::string{}};
    FramePipeline pipeline;
    EnvCubemap envCube{sky};
    meshes.setEnvMap(envCube.textureId(), envCube.maxLod());
    SunShadow shadow;

    // Display-space target (the scene flips Z).
    const Vec3 dt = flipZ(c);
    const ::Vector3 target{static_cast<float>(dt.x), static_cast<float>(dt.y),
                           static_cast<float>(dt.z)};

    // A yaw sweep at the framed distance is the direct test for symptom 2 ("rotating
    // drops half"): every angle must show a full, complete station. Plus an inside
    // view for the near-plane / cull regime.
    const std::vector<Shot> shots{
        {"framed_0", 2.5, 0.0},     {"framed_90", 2.5, 90.0}, {"framed_180", 2.5, 180.0},
        {"framed_270", 2.5, 270.0}, {"inside", 0.4, 35.0},
    };
    for (const Shot& shot : shots) {
      const double dist = radius * shot.distFactor;
      const double yaw = shot.yawDeg * static_cast<double>(DEG2RAD);
      const double pitch = 0.45;  // ~26 deg, a 3/4 elevation
      const Vec3 dirN{std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                      std::cos(pitch) * std::cos(yaw)};
      const ::Camera3D camera{::Vector3{target.x + static_cast<float>(dirN.x * dist),
                                        target.y + static_cast<float>(dirN.y * dist),
                                        target.z + static_cast<float>(dirN.z * dist)},
                              target, ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};

      const RenderStats st = lodStats(*station, *catalog, camera);
      const double fps = renderAndCapture(*station, *catalog, camera, meshes, sky, pipeline, shadow,
                                          outPrefix + "_" + shot.label + ".png");
      std::printf("megashot: [%-7s] dist=%.0f  total=%d culled=%d detailed=%d box=%d  fps=%.1f\n",
                  shot.label, dist, st.total, st.culled, st.drawnDetailed, st.drawnBox, fps);
    }
  }
  CloseWindow();
  return 0;
}

}  // namespace x4sb::editor

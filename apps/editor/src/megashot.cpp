#include "megashot.hpp"

#include "app_paths.hpp"
#include "mesh_cache.hpp"
#include "render.hpp"

#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/planio/plan.hpp"

#include "raylib.h"

#include <cmath>
#include <cstdio>
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
                        const ::Camera3D& camera, MeshCache& meshes, const std::string& outPath) {
  constexpr int kFrames = 40;
  for (int i = 0; i < 3; ++i) {  // warm-up (mesh upload, shader) excluded from timing
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    drawScene(station, catalog, camera, meshes, /*showGizmos=*/true, /*showMeshes=*/true);
    EndDrawing();
  }
  const double t0 = GetTime();
  for (int i = 0; i < kFrames; ++i) {
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    drawScene(station, catalog, camera, meshes, /*showGizmos=*/true, /*showMeshes=*/true);
    EndDrawing();
  }
  const double dt = GetTime() - t0;
  TakeScreenshot(outPath.c_str());  // raylib writes basename into the working dir
  return dt > 0 ? kFrames / dt : 0.0;
}

}  // namespace

int runMegaShot(const std::string& planPath, const std::string& outPrefix) {
  const std::optional<std::string> catalogPath = findCatalogJson();
  if (!catalogPath) {
    std::fprintf(stderr, "megashot: could not find asset-cache/catalog.json\n");
    return 1;
  }
  std::optional<ModuleCatalog> catalog = ModuleCatalog::loadFromFile(*catalogPath);
  if (!catalog) {
    std::fprintf(stderr, "megashot: failed to load catalog %s\n", catalogPath->c_str());
    return 1;
  }
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

  InitWindow(kScreenW, kScreenH, "X4 Station Builder - megashot");
  // No SetTargetFPS: leave the frame rate uncapped so renderAndCapture's timing
  // reflects real render cost, not a 60 Hz cap.
  {
    MeshCache meshes{std::filesystem::path(*catalogPath).parent_path()};

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
      const ::Camera3D camera{
          ::Vector3{target.x + static_cast<float>(dirN.x * dist),
                    target.y + static_cast<float>(dirN.y * dist),
                    target.z + static_cast<float>(dirN.z * dist)},
          target, ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};

      const RenderStats st = lodStats(*station, *catalog, camera);
      const double fps = renderAndCapture(*station, *catalog, camera, meshes,
                                          outPrefix + "_" + shot.label + ".png");
      std::printf("megashot: [%-7s] dist=%.0f  total=%d culled=%d detailed=%d box=%d  fps=%.1f\n",
                  shot.label, dist, st.total, st.culled, st.drawnDetailed, st.drawnBox, fps);
    }
  }
  CloseWindow();
  return 0;
}

}  // namespace x4sb::editor

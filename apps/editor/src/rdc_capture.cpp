#include "rdc_capture.hpp"

#include "app_paths.hpp"
#include "mesh_cache.hpp"
#include "rdc_api.hpp"
#include "render.hpp"

#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/planio/plan.hpp"

#include "raylib.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace x4sb::editor {
namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;
constexpr int kWarmup = 10;  // upload meshes / compile shaders before the captured frame

// Draw one representative framed view of the whole station.
void drawFramed(const Station& station, const ModuleCatalog& catalog, MeshCache& meshes,
                const Vec3& displayTarget, double radius) {
  const ::Vector3 target{static_cast<float>(displayTarget.x), static_cast<float>(displayTarget.y),
                         static_cast<float>(displayTarget.z)};
  const double dist = std::max(radius, 1.0) * 1.8;  // whole station in view
  constexpr double kYaw = 0.6;
  constexpr double kPitch = 0.45;
  const Vec3 dir{std::cos(kPitch) * std::sin(kYaw), std::sin(kPitch), std::cos(kPitch) * std::cos(kYaw)};
  const ::Camera3D camera{::Vector3{target.x + static_cast<float>(dir.x * dist),
                                    target.y + static_cast<float>(dir.y * dist),
                                    target.z + static_cast<float>(dir.z * dist)},
                          target, ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};
  BeginDrawing();
  ClearBackground(::Color{30, 30, 38, 255});
  drawScene(station, catalog, camera, meshes, /*showGizmos=*/true, /*showMeshes=*/true);
  EndDrawing();
}

}  // namespace

int runRdcCapture(const std::string& planPath) {
  const std::optional<std::string> catalogPath = findCatalogJson();
  if (!catalogPath) {
    std::fprintf(stderr, "rdc: could not find asset-cache/catalog.json\n");
    return 1;
  }
  std::optional<ModuleCatalog> catalog = ModuleCatalog::loadFromFile(*catalogPath);
  if (!catalog) {
    std::fprintf(stderr, "rdc: failed to load catalog %s\n", catalogPath->c_str());
    return 1;
  }
  std::ifstream in(planPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "rdc: cannot open plan %s\n", planPath.c_str());
    return 1;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::optional<Station> station = importPlanXml(ss.str());
  if (!station) {
    std::fprintf(stderr, "rdc: failed to parse plan %s\n", planPath.c_str());
    return 1;
  }

  const StationBounds bounds = stationBounds(*station, *catalog);
  std::printf("rdc: modules=%zu radius=%.0f  under_renderdoc=%s\n", station->modules().size(),
              bounds.radius, rdc::available() ? "yes" : "no");
  if (!rdc::available())
    std::printf("rdc: not launched under RenderDoc — rendering only. Use tools/rdc-capture.ps1 to "
                "produce a .rdc.\n");

  InitWindow(kScreenW, kScreenH, "X4 Station Builder - rdc");
  {
    MeshCache meshes{std::filesystem::path(*catalogPath).parent_path()};
    const Vec3 displayTarget = flipZ(bounds.center);  // scene flips Z

    for (int i = 0; i < kWarmup && !WindowShouldClose(); ++i)
      drawFramed(*station, *catalog, meshes, displayTarget, bounds.radius);

    // Bracket exactly one frame: RenderDoc records every GL call between these.
    rdc::startFrameCapture();
    drawFramed(*station, *catalog, meshes, displayTarget, bounds.radius);
    rdc::endFrameCapture();
  }
  CloseWindow();
  std::printf("rdc: captured 1 frame%s\n", rdc::available() ? "" : " (no-op; not under RenderDoc)");
  return 0;
}

}  // namespace x4sb::editor

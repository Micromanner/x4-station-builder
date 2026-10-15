#include "snaptest.hpp"

#include "bloom.hpp"
#include "mesh_cache.hpp"
#include "native_assets.hpp"
#include "raylib.h"
#include "raylib_convert.hpp"
#include "render.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/data/types.hpp"
#include "x4sb/document/commands.hpp"
#include "x4sb/document/station.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/snap/snap.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace x4sb::editor {
namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;
// Preferred test module: a 6-connector cross hub (all 6 meshes exist on disk).
constexpr const char* kPreferredMacro = "struct_arg_cross_01_macro";

// True if every meshRef of `def` has its lod0 XMF in the game archives.
// A module with no meshRefs cannot be visually validated, so it counts as false.
[[nodiscard]] bool allMeshesExist(const ModuleDef& def, const NativeAssets& native) {
  if (def.meshRefs.empty()) return false;
  for (const MeshRef& ref : def.meshRefs) {
    if (!native.archive->contains(ref.xmfPath)) return false;
  }
  return true;
}

// Pick the cross hub if present + complete, else the first Connector-category
// module with >=2 connection points whose meshes all exist. nullptr if none.
[[nodiscard]] const ModuleDef* pickTestModule(const ModuleCatalog& catalog,
                                              const NativeAssets& native) {
  if (const ModuleDef* preferred = catalog.find(kPreferredMacro);
      preferred != nullptr && preferred->connectionPoints.size() >= 2 &&
      allMeshesExist(*preferred, native)) {
    return preferred;
  }
  // Deterministic fallback: scan modules in sorted id order so the choice is stable.
  std::vector<const ModuleDef*> defs;
  defs.reserve(catalog.all().size());
  for (const auto& entry : catalog.all()) defs.push_back(&entry.second);
  std::sort(defs.begin(), defs.end(),
            [](const ModuleDef* a, const ModuleDef* b) { return a->id < b->id; });
  for (const ModuleDef* def : defs) {
    if (def->category != Category::Connector) continue;
    if (def->connectionPoints.size() < 2) continue;
    if (allMeshesExist(*def, native)) return def;
  }
  return nullptr;
}

// Format a transform as position + quaternion (X4 native space).
[[nodiscard]] std::string formatTransform(const char* label, const Transform& t) {
  std::array<char, 160> buf{};
  std::snprintf(buf.data(), buf.size(),
                "  %s: pos=(%.3f, %.3f, %.3f) quat(w,x,y,z)=(%.4f, %.4f, %.4f, %.4f)\n", label,
                t.position.x, t.position.y, t.position.z, t.rotation.w, t.rotation.x, t.rotation.y,
                t.rotation.z);
  return std::string(buf.data());
}

// Render one full frame of `station` and capture it. showMeshes selects wireframe
// meshes vs. AABB boxes. A few warm-up frames precede the shot so the captured
// framebuffer is clean.
void renderAndCapture(const Station& station, const ModuleCatalog& catalog,
                      const ::Camera3D& camera, MeshCache& meshes, FramePipeline& pipeline,
                      bool showMeshes, const std::string& outPath) {
  for (int i = 0; i < 3; ++i) {
    BeginDrawing();
    pipeline.begin();
    ClearBackground(::Color{30, 30, 38, 255});
    // allConnectors: this harness exists to eyeball the snap/handedness convention,
    // so show every module's connector markers + gizmo (the editor gates them).
    drawScene(station, catalog, camera, meshes, /*showGizmos=*/true, showMeshes,
              /*allConnectors=*/true);
    pipeline.end();
    EndDrawing();
  }
  TakeScreenshot(outPath.c_str());  // captures the just-presented framebuffer
}

}  // namespace

int runSnapTest(const NativeAssets& native, const std::string& outPrefix) {
  const ModuleCatalog* catalog = &native.catalog;

  const ModuleDef* def = pickTestModule(*catalog, native);
  if (def == nullptr) {
    std::fprintf(stderr,
                 "snaptest: no suitable module (need %s or a Connector module with >=2 "
                 "connectors whose meshes all exist)\n",
                 kPreferredMacro);
    return 1;
  }

  // GL context required before LoadModel (MeshCache) and TakeScreenshot.
  InitWindow(kScreenW, kScreenH, "X4 Station Builder - snaptest");
  SetTargetFPS(60);

  {  // Inner scope: MeshCache destructs (UnloadModel) before CloseWindow below.
    MeshCache meshes{native};
    FramePipeline pipeline;

    // Build the station with the REAL command + snap APIs.
    Station station;
    UndoStack undo;
    // Instance 1: root module at the identity transform.
    undo.execute(station, std::make_unique<PlaceModuleCommand>(def->id, Transform{}));
    const InstanceId rootId = station.modules().front().instanceId;

    // World position of instance 1's first connector. At identity, apply() returns
    // the connector's local position unchanged.
    const ConnectionPoint& cp0 = def->connectionPoints.front();
    const Vec3 connWorld = apply(station.modules().front().worldTransform, cp0.localPosition);

    // The report is accumulated into a string and written to BOTH stdout and a
    // sibling <outPrefix>_report.txt. The editor links -mwindows in Release (no
    // console), so stdout alone would be lost; the file is the reliable channel.
    std::string report;

    // Instance 2: snap to that connector via the real find->solve->collide path
    // (exercises kMate, the convention under visual test). Radius generous enough
    // to catch the connector regardless of module scale.
    constexpr double kSnapRadius = 2000.0;
    std::unique_ptr<Command> snapCmd =
        makeSnapPlacement(*def, connWorld, station, *catalog, kSnapRadius);
    if (snapCmd) {
      undo.execute(station, std::move(snapCmd));
    } else {
      std::array<char, 200> buf{};
      std::snprintf(buf.data(), buf.size(),
                    "snaptest: makeSnapPlacement found no free compatible target within %.0f of "
                    "(%.1f, %.1f, %.1f) (or it would collide) - instance 1 alone\n",
                    kSnapRadius, connWorld.x, connWorld.y, connWorld.z);
      report += buf.data();
    }

    // Fixed 3/4-angle camera framing the pair. The scene geometry has its Z
    // flipped at load to display space, so the camera lives in display space — negate
    // the target's Z (the geometry's Z axis is mirrored on screen).
    Vec3 midX4{0, 0, 0};
    for (const auto& pm : station.modules()) midX4 = midX4 + pm.worldTransform.position;
    midX4 = midX4 * (1.0 / static_cast<double>(station.modules().size()));
    const ::Camera3D camera{::Vector3{900.0f, 700.0f, 900.0f},  // X4 units (~1.5 km out, 3/4 view)
                            toRl(flipZ(midX4)),                 // display-space target (flip Z)
                            ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};

    const std::string meshPath = outPrefix + "_mesh.png";
    const std::string boxPath = outPrefix + "_box.png";
    renderAndCapture(station, *catalog, camera, meshes, pipeline, /*showMeshes=*/true, meshPath);
    renderAndCapture(station, *catalog, camera, meshes, pipeline, /*showMeshes=*/false, boxPath);

    // Report what was placed + where the screenshots went.
    std::array<char, 256> line{};
    std::snprintf(line.data(), line.size(), "snaptest: module=%s instances=%zu\n", def->id.c_str(),
                  station.modules().size());
    report += line.data();
    std::snprintf(line.data(), line.size(), "  instance 1 (root, id=%llu):\n",
                  static_cast<unsigned long long>(rootId));
    report += line.data();
    report += formatTransform("transform", station.modules().front().worldTransform);
    if (station.modules().size() >= 2) {
      const PlacedModule& second = station.modules().back();
      std::snprintf(line.data(), line.size(), "  instance 2 (snapped, id=%llu):\n",
                    static_cast<unsigned long long>(second.instanceId));
      report += line.data();
      report += formatTransform("transform", second.worldTransform);
    }
    report += "  screenshots: " + meshPath + " , " + boxPath + "\n";

    // Reliable channel: write the report beside the PNGs. Also echo to stdout for
    // a console-subsystem build / redirected run.
    const std::string reportPath = outPrefix + "_report.txt";
    std::ofstream out(reportPath);
    if (out) out << report;
    report += "  report: " + reportPath + "\n";
    std::fputs(report.c_str(), stdout);
  }

  CloseWindow();  // after MeshCache destruction (UnloadModel needs the context)
  return 0;
}

}  // namespace x4sb::editor

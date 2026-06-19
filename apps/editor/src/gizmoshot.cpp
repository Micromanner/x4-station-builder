#include "gizmoshot.hpp"

#include "app_paths.hpp"
#include "camera_math.hpp"  // zoomTowardCursor, cameraBasis
#include "input.hpp"        // gizmoScaleFor
#include "mesh_cache.hpp"
#include "raylib_convert.hpp"  // toRl
#include "render.hpp"

#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/data/types.hpp"
#include "x4sb/document/station.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/editor_state.hpp"
#include "x4sb/editorcore/gizmo.hpp"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace x4sb::editor {
namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;
constexpr const char* kPreferredMacro = "struct_arg_cross_01_macro";

[[nodiscard]] bool allMeshesExist(const ModuleDef& def, const std::filesystem::path& assetRoot) {
  if (def.meshRefs.empty()) return false;
  for (const MeshRef& ref : def.meshRefs) {
    std::error_code ec;
    if (!std::filesystem::exists(assetRoot / ref.gltfPath, ec) || ec) return false;
  }
  return true;
}

// Prefer the 6-connector cross hub (visually busy, good for the eyeball test); else
// the first id-sorted module whose meshes all exist on disk.
[[nodiscard]] const ModuleDef* pickModule(const ModuleCatalog& catalog,
                                          const std::filesystem::path& assetRoot) {
  if (const ModuleDef* preferred = catalog.find(kPreferredMacro);
      preferred != nullptr && allMeshesExist(*preferred, assetRoot)) {
    return preferred;
  }
  std::vector<const ModuleDef*> defs;
  defs.reserve(catalog.all().size());
  for (const auto& entry : catalog.all()) defs.push_back(&entry.second);
  std::sort(defs.begin(), defs.end(),
            [](const ModuleDef* a, const ModuleDef* b) { return a->id < b->id; });
  for (const ModuleDef* def : defs)
    if (allMeshesExist(*def, assetRoot)) return def;
  return nullptr;
}

// Render one frame of the editor state (gizmo included) and capture it.
void shoot(const EditorState& state, const ::Camera3D& camera, MeshCache& meshes,
           const std::string& outPath) {
  for (int i = 0; i < 3; ++i) {  // warm-up so the captured framebuffer is clean
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    drawScene(state, camera, meshes, /*showGizmos=*/true, /*showMeshes=*/true);
    EndDrawing();
  }
  TakeScreenshot(outPath.c_str());
}

}  // namespace

// Build the editor's orbit ::Camera3D for a given pose (mirrors OrbitCamera::rebuild
// + the fovy=60 perspective set in OrbitCamera's ctor).
[[nodiscard]] ::Camera3D orbitCam(::Vector3 target, double dist, double yaw, double pitch) {
  const double cp = std::cos(pitch);
  return ::Camera3D{
      ::Vector3{target.x + static_cast<float>(dist * cp * std::sin(yaw)),
                target.y + static_cast<float>(dist * std::sin(pitch)),
                target.z + static_cast<float>(dist * cp * std::cos(yaw))},
      target, ::Vector3{0.0F, 1.0F, 0.0F}, 60.0F, CAMERA_PERSPECTIVE};
}

// The gizmo's true on-screen size: the longest axis arm in PIXELS, measured with
// raylib's own GetWorldToScreen (the exact projection the GPU uses). The handle is
// drawn at module.position + axisDir*scale (X4-native) inside the (1,1,-1) scene
// flip, so its rendered position is flipZ(...) — project that.
[[nodiscard]] double gizmoArmPixels(const EditorState& state, const ::Camera3D& cam, double scale) {
  const PlacedModule* m = state.station().find(*state.selected());
  const Vec3 o = m->worldTransform.position;
  const ::Vector2 po = GetWorldToScreen(toRl(flipZ(o)), cam);
  double maxd = 0.0;
  for (const GizmoHandle h : {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ}) {
    const ::Vector2 pt = GetWorldToScreen(toRl(flipZ(o + gizmoAxisDir(h) * scale)), cam);
    maxd = std::max(maxd, static_cast<double>(Vector2Distance(po, pt)));
  }
  return maxd;
}

// Replay a real zoom gesture through the editor's own camera + gizmoScaleFor + raylib
// projection, logging the gizmo's true pixel size at each step. `cursorNdcX` parks the
// mouse at a fixed screen offset while wheeling (0 == centred). This is the ground-truth
// measurement of "does the handle stay the same size as I zoom?".
void sweep(const EditorState& state, ::Vector3 moduleDisp, double cursorNdcX, double wheel,
           const char* label) {
  constexpr double kTanHalfFov = 0.5773502691896257;  // tan(60deg/2)
  const double aspect = static_cast<double>(GetScreenWidth()) / GetScreenHeight();
  const double yaw = 0.6;
  const double pitch = 0.45;
  ::Vector3 target = moduleDisp;
  double distance = 40.0;
  std::printf("\n[%s] cursorNdcX=%.2f wheel=%+.0f  (orbit fovy=60, %dx%d)\n", label, cursorNdcX,
              wheel, GetScreenWidth(), GetScreenHeight());
  std::printf("  %9s %9s %9s %9s %9s\n", "dist", "depth", "eyeDist", "scale", "armPx");
  double minPx = 1e30;
  double maxPx = 0.0;
  for (int step = 0; step < 80; ++step) {  // ~80 steps reaches the 40000 zoom clamp ("fully out")
    const ::Camera3D cam = orbitCam(target, distance, yaw, pitch);
    const double scale = gizmoScaleFor(cam, state);
    const x4sb::editor::CameraBasis basis =
        x4sb::editor::cameraBasis(toVec3(cam.target) - toVec3(cam.position), Vec3{0, 1, 0});
    const double depth = dot(toVec3(moduleDisp) - toVec3(cam.position), basis.forward);
    const double eyeDist = static_cast<double>(Vector3Distance(cam.position, moduleDisp));
    const double armPx = gizmoArmPixels(state, cam, scale);
    std::printf("  %9.1f %9.1f %9.1f %9.1f %9.1f\n", distance, depth, eyeDist, scale, armPx);
    minPx = std::min(minPx, armPx);
    maxPx = std::max(maxPx, armPx);

    const Vec3 rayDir = x4sb::editor::normalized(
        basis.forward + basis.right * (cursorNdcX * kTanHalfFov * aspect));
    const double k = 1.0 - wheel * 0.1;
    const x4sb::editor::ZoomResult z = x4sb::editor::zoomTowardCursor(
        toVec3(cam.target), distance, basis.forward, toVec3(cam.position), rayDir, k, 2.0, 40000.0);
    target = toRl(z.target);
    distance = z.distance;
  }
  std::printf("  armPx range: %.1f .. %.1f  (ratio %.2fx)\n", minPx, maxPx, maxPx / minPx);
}

int runGizmoSweep() {
  const std::optional<std::string> catalogPath = findCatalogJson();
  if (!catalogPath) {
    std::fprintf(stderr, "gizmosweep: no catalog.json\n");
    return 1;
  }
  std::optional<ModuleCatalog> catalog = ModuleCatalog::loadFromFile(*catalogPath);
  if (!catalog) {
    std::fprintf(stderr, "gizmosweep: failed to load catalog\n");
    return 1;
  }
  InitWindow(kScreenW, kScreenH, "gizmosweep");
  {
    EditorState state(*catalog);
    Station station;
    PlacedModule pm;
    pm.instanceId = 1;
    pm.defId = catalog->all().begin()->first;  // any module; gizmo size is module-independent
    station.add(pm);
    state.loadStation(std::move(station));
    state.selectByRay(Vec3{0, 1e6, 0}, Vec3{0, -1, 0});

    const ::Vector3 moduleDisp = toRl(flipZ(Vec3{0, 0, 0}));  // module at origin
    sweep(state, moduleDisp, /*cursorNdcX=*/0.0, /*wheel=*/-1.0, "zoom OUT, cursor centred");
    sweep(state, moduleDisp, /*cursorNdcX=*/0.30, /*wheel=*/-1.0, "zoom OUT, cursor 30% off");
    sweep(state, moduleDisp, /*cursorNdcX=*/0.0, /*wheel=*/+1.0, "zoom IN, cursor centred");
  }
  CloseWindow();
  return 0;
}

int runGizmoShot(const std::string& outPrefix) {
  const std::optional<std::string> catalogPath = findCatalogJson();
  if (!catalogPath) {
    std::fprintf(stderr, "gizmoshot: could not find asset-cache/catalog.json\n");
    return 1;
  }
  std::optional<ModuleCatalog> catalog = ModuleCatalog::loadFromFile(*catalogPath);
  if (!catalog) {
    std::fprintf(stderr, "gizmoshot: failed to load catalog %s\n", catalogPath->c_str());
    return 1;
  }
  const std::filesystem::path assetRoot = std::filesystem::path(*catalogPath).parent_path();
  const ModuleDef* def = pickModule(*catalog, assetRoot);
  if (def == nullptr) {
    std::fprintf(stderr, "gizmoshot: no module with on-disk meshes found\n");
    return 1;
  }

  InitWindow(kScreenW, kScreenH, "X4 Station Builder - gizmoshot");
  SetTargetFPS(60);
  {
    MeshCache meshes{assetRoot};

    // One module at the identity transform, then select it (the gizmo only draws
    // for a selection). loadStation resets selection; selectByRay re-picks it.
    EditorState state(*catalog);
    Station station;
    PlacedModule pm;
    pm.instanceId = 1;
    pm.defId = def->id;
    station.add(pm);
    state.loadStation(std::move(station));
    const std::optional<InstanceId> sel = state.selectByRay(Vec3{0, 1e6, 0}, Vec3{0, -1, 0});

    // Frame the module from a 3/4 angle. Scene flips Z, so the camera lives in
    // display space (negate the X4 center's Z).
    const StationBounds b = stationBounds(state.station(), state.catalog());
    const double r = b.radius > 1.0 ? b.radius : 1.0;
    const ::Vector3 target = toRl(flipZ(b.center));
    const double dist = r * 2.5;
    const double pitch = 0.45;  // ~26 deg elevation
    const double yaw = 0.6;
    const ::Camera3D camera{
        ::Vector3{target.x + static_cast<float>(std::cos(pitch) * std::sin(yaw) * dist),
                  target.y + static_cast<float>(std::sin(pitch) * dist),
                  target.z + static_cast<float>(std::cos(pitch) * std::cos(yaw) * dist)},
        target, ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};

    const double scale = gizmoScaleFor(camera, state);
    std::printf("gizmoshot: module=%s selected=%s radius=%.1f gizmoScale=%.1f\n", def->id.c_str(),
                sel ? "yes" : "NO", r, scale);

    // (1) Idle gizmo: rings, plane handles, arrows, orientation triad all visible.
    shoot(state, camera, meshes, outPrefix + "_idle.png");

    // (2) Mid translate-drag: grab +X (ray down onto the axis at 0.6*scale), drag it
    // out by ~1.5*radius. The selected mesh must move LIVE to the preview pose.
    const double px = 0.6 * scale;
    if (state.beginGizmoDrag(Vec3{px, 1e6, 0}, Vec3{0, -1, 0}, scale)) {
      const double moved = px + 1.5 * r;
      state.updateGizmoDrag(Vec3{moved, 1e6, 0}, Vec3{0, -1, 0}, /*forceFree=*/true);
      shoot(state, camera, meshes, outPrefix + "_move.png");
      state.cancelGizmoDrag();
    } else {
      std::printf("gizmoshot: translate grab MISSED (no _move.png)\n");
    }

    // (3) Mid rotate-drag: grab the RotY ring (XZ plane, radius scale) and sweep to
    // +X. The selected mesh must rotate LIVE in place.
    const double s = scale * 0.70710678;
    if (state.beginGizmoDrag(Vec3{s, 1e6, s}, Vec3{0, -1, 0}, scale)) {
      state.updateGizmoDrag(Vec3{scale, 1e6, 0}, Vec3{0, -1, 0}, /*forceFree=*/true);
      shoot(state, camera, meshes, outPrefix + "_rotate.png");
      state.cancelGizmoDrag();
    } else {
      std::printf("gizmoshot: rotate grab MISSED (no _rotate.png)\n");
    }

    std::printf("gizmoshot: wrote %s_{idle,move,rotate}.png\n", outPrefix.c_str());
  }
  CloseWindow();
  return 0;
}

}  // namespace x4sb::editor

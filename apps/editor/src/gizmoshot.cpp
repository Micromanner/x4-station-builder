#include "gizmoshot.hpp"

#include "app_paths.hpp"
#include "input.hpp"  // gizmoScaleFor
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

// x4sb-editor — the interactive station builder (parent §4B unit 5). Thin raylib
// shell: load the catalog, then per frame map input into EditorState (render-free
// logic in libs/editorcore) and draw it. All scene geometry is in X4 native space;
// the renderer applies the single (1,1,-1) handedness flip.
#include "app_paths.hpp"
#include "input.hpp"
#include "mesh_cache.hpp"
#include "orbit_camera.hpp"
#include "plan_io.hpp"
#include "render.hpp"
#include "snaptest.hpp"

#include "x4sb/data/catalog.hpp"
#include "x4sb/editorcore/editor_state.hpp"

#include "raylib.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace {
constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

// Combined center + radius of the whole station (for the F-frame key).
void stationBounds(const x4sb::EditorState& s, ::Vector3& centerOut, float& radiusOut) {
  centerOut = ::Vector3{0, 0, 0};
  radiusOut = 20.0f;
  const auto& mods = s.station().modules();
  if (mods.empty()) return;
  x4sb::AABB box{mods.front().worldTransform.position, mods.front().worldTransform.position};
  for (const auto& pm : mods) {
    const x4sb::ModuleDef* d = s.defFor(pm.defId);
    if (d == nullptr) continue;
    const x4sb::AABB wb = x4sb::worldAabb(d->aabb, pm.worldTransform);
    box = x4sb::merge(box, wb);
  }
  const x4sb::Vec3 c = (box.min + box.max) * 0.5;
  centerOut = ::Vector3{static_cast<float>(c.x), static_cast<float>(c.y),
                        static_cast<float>(-c.z)};  // display space (flip Z)
  const x4sb::Vec3 ext = box.max - box.min;
  radiusOut = static_cast<float>(x4sb::length(ext) * 0.5);
}

// Error loop: catalog not found / unparseable. Show what we tried until closed.
int runCatalogError() {
  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawText("Could not find asset-cache/catalog.json.", 20, 20, 20, MAROON);
    DrawText("Launch the editor from the project root (the folder that contains", 20, 52, 18,
             DARKGRAY);
    DrawText("asset-cache/), or generate the cache via the pipeline first.", 20, 76, 18, DARKGRAY);
    DrawText(TextFormat("Working dir: %s", GetWorkingDirectory()), 20, 116, 16, GRAY);
    DrawText(TextFormat("Exe dir:     %s", GetApplicationDirectory()), 20, 140, 16, GRAY);
    EndDrawing();
  }
  CloseWindow();
  return 1;
}
}  // namespace

int main(int argc, char** argv) {
  // Hidden visual-regression harness: `--snaptest <outPrefix>` renders a snapped
  // pair, screenshots it, and exits (no interactive window). It owns its own
  // InitWindow/CloseWindow, so dispatch BEFORE the interactive InitWindow below.
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--snaptest") {
      return x4sb::editor::runSnapTest(std::string(argv[i + 1]));
    }
  }

  InitWindow(kScreenW, kScreenH, "X4 Station Builder");
  SetTargetFPS(60);

  const std::optional<std::string> catalogPath = x4sb::editor::findCatalogJson();
  std::optional<x4sb::ModuleCatalog> catalog =
      catalogPath ? x4sb::ModuleCatalog::loadFromFile(*catalogPath) : std::nullopt;
  if (!catalog) return runCatalogError();

  x4sb::EditorState state(*catalog);

  // MeshCache owns raylib Models, so it must be constructed after InitWindow (GL
  // context required by LoadModel) and destroyed before CloseWindow (UnloadModel
  // also needs the context). It and the render loop live in this inner block so
  // the cache's destructor runs at the block's close — before the explicit
  // CloseWindow() below. Asset-cache root = the directory containing catalog.json.
  {
    x4sb::editor::MeshCache meshes{std::filesystem::path(*catalogPath).parent_path()};
    x4sb::editor::OrbitCamera cam;
    bool showGizmos = true;
    bool showMeshes = true;  // render-only state; not in EditorState (that's render-free)
    std::string toast;
    double toastUntil = 0.0;

    while (!WindowShouldClose()) {
      cam.update();
      x4sb::editor::handleKeys(state);
      if (IsKeyPressed(KEY_G)) showGizmos = !showGizmos;
      if (IsKeyPressed(KEY_M)) showMeshes = !showMeshes;
      if (IsKeyPressed(KEY_F)) {
        ::Vector3 c{};
        float r = 20.0f;
        stationBounds(state, c, r);
        cam.frame(c, r);
      }
      if (std::optional<std::string> msg = x4sb::editor::handlePlanIoKeys(state)) {
        toast = *msg;
        toastUntil = GetTime() + 4.0;
      }

      // Mouse ray in X4 space drives the ghost preview every frame.
      x4sb::Vec3 ro{};
      x4sb::Vec3 rd{};
      x4sb::editor::mouseRayX4(cam.camera(), ro, rd);
      state.updateGhost(ro, rd);

      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        // Commit a valid ghost; otherwise treat the click as a selection.
        const bool placed = state.ghost().has_value() && state.ghost()->valid &&
                            state.commitGhost().has_value();
        if (!placed) state.selectByRay(ro, rd);
      }

      BeginDrawing();
      ClearBackground(::Color{30, 30, 38, 255});
      x4sb::editor::drawScene(state, cam.camera(), meshes, showGizmos, showMeshes);
      x4sb::editor::drawHud(state, kScreenW, kScreenH, showGizmos);
      if (GetTime() < toastUntil) x4sb::editor::drawToast(toast, kScreenH);
      EndDrawing();
    }
  }  // meshes destroyed here (UnloadModel) before CloseWindow

  CloseWindow();
  return 0;
}

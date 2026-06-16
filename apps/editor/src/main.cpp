// x4sb-editor — the interactive station builder (parent §4B unit 5). Thin raylib
// shell: load the catalog, then per frame map input into EditorState (render-free
// logic in libs/editorcore) and draw it. All scene geometry is in X4 native space;
// the renderer applies the single (1,1,-1) handedness flip.
#include "input.hpp"
#include "orbit_camera.hpp"
#include "render.hpp"

#include "x4sb/data/catalog.hpp"
#include "x4sb/editorcore/editor_state.hpp"

#include "raylib.h"

#include <array>
#include <cstdio>
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

// Locate asset-cache/catalog.json regardless of the launch directory (CWD or a
// double-clicked exe). Searches from both the working directory and the
// executable's directory, walking up a few levels so a build/<preset>/bin/ exe
// still finds the repo-root cache. Returns the first existing path, or nullopt.
std::optional<std::string> findCatalogJson() {
  const std::string rel = "asset-cache/catalog.json";
  const std::array<std::string, 2> bases{std::string{}, std::string{GetApplicationDirectory()}};
  const std::array<std::string, 5> ups{"", "../", "../../", "../../../", "../../../../"};
  for (const auto& base : bases) {
    for (const auto& up : ups) {
      const std::string path = base + up + rel;
      if (FileExists(path.c_str())) return path;
    }
  }
  return std::nullopt;
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

int main() {
  InitWindow(kScreenW, kScreenH, "X4 Station Builder");
  SetTargetFPS(60);

  const std::optional<std::string> catalogPath = findCatalogJson();
  std::optional<x4sb::ModuleCatalog> catalog =
      catalogPath ? x4sb::ModuleCatalog::loadFromFile(*catalogPath) : std::nullopt;
  if (!catalog) return runCatalogError();

  x4sb::EditorState state(*catalog);
  x4sb::editor::OrbitCamera cam;
  bool showGizmos = true;

  while (!WindowShouldClose()) {
    cam.update();
    x4sb::editor::handleKeys(state);
    if (IsKeyPressed(KEY_G)) showGizmos = !showGizmos;
    if (IsKeyPressed(KEY_F)) {
      ::Vector3 c{};
      float r = 20.0f;
      stationBounds(state, c, r);
      cam.frame(c, r);
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
    x4sb::editor::drawScene(state, cam.camera(), showGizmos);
    x4sb::editor::drawHud(state, kScreenW, kScreenH, showGizmos);
    EndDrawing();
  }

  CloseWindow();
  return 0;
}

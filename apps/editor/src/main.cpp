// x4sb-editor — the interactive station builder (parent §4B unit 5). Thin raylib
// shell: load the catalog, then per frame map input into EditorState (render-free
// logic in libs/editorcore) and draw it. All scene geometry is in X4 native space;
// the renderer applies the single (1,1,-1) handedness flip.
#include "app_paths.hpp"
#include "connbench.hpp"
#include "gizmoshot.hpp"
#include "input.hpp"
#include "loadbench.hpp"
#include "mesh_cache.hpp"
#include "orbit_camera.hpp"
#include "megashot.hpp"
#include "mesh_load.hpp"
#include "plan_io.hpp"
#include "iprofile.hpp"
#include "profile.hpp"
#include "profiling.hpp"
#include "rdc_capture.hpp"
#include "render.hpp"
#include "snaptest.hpp"

#include "x4sb/data/catalog.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/editor_state.hpp"

#include "raylib.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace {
constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

// X4-space bounds -> the display-space center + radius the camera frames. The
// single flip + cast site shared by every F-frame focus path.
void toFraming(const x4sb::editor::StationBounds& b, ::Vector3& centerOut, float& radiusOut) {
  const x4sb::Vec3 d = x4sb::flipZ(b.center);  // display space
  centerOut = ::Vector3{static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)};
  radiusOut = static_cast<float>(b.radius);
}

// Display-space center + radius of the whole station (for the F-frame key). An
// empty station keeps a sane default framing.
void stationBounds(const x4sb::EditorState& s, ::Vector3& centerOut, float& radiusOut) {
  if (s.station().modules().empty()) {
    centerOut = ::Vector3{0, 0, 0};
    radiusOut = 20.0f;
    return;
  }
  toFraming(x4sb::editor::stationBounds(s.station(), s.catalog()), centerOut, radiusOut);
}

// Display-space center + radius of the currently selected module. Returns false
// (leaving the outputs untouched) when there is no resolvable selection.
[[nodiscard]] bool focusSelection(const x4sb::EditorState& s, ::Vector3& centerOut,
                                  float& radiusOut) {
  const std::optional<x4sb::InstanceId> sel = s.selected();
  if (!sel) return false;
  const x4sb::PlacedModule* m = s.station().find(*sel);
  if (m == nullptr) return false;
  const x4sb::ModuleDef* def = s.catalog().find(m->defId);
  if (def == nullptr) return false;
  toFraming(x4sb::editor::boundsOf(x4sb::worldAabb(def->aabb, m->worldTransform)), centerOut,
            radiusOut);
  return true;
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
    if (std::string(argv[i]) == "--megashot" && i + 2 < argc) {
      return x4sb::editor::runMegaShot(std::string(argv[i + 1]), std::string(argv[i + 2]));
    }
    if (std::string(argv[i]) == "--gizmoshot") {
      return x4sb::editor::runGizmoShot(std::string(argv[i + 1]));
    }
    if (std::string(argv[i]) == "--gizmosweep") {
      return x4sb::editor::runGizmoSweep();
    }
    if (std::string(argv[i]) == "--profile" && i + 2 < argc) {
      return x4sb::editor::runProfile(std::string(argv[i + 1]), std::atoi(argv[i + 2]));
    }
    if (std::string(argv[i]) == "--iprofile" && i + 2 < argc) {
      return x4sb::editor::runInteractiveProfile(std::string(argv[i + 1]), std::atoi(argv[i + 2]));
    }
    if (std::string(argv[i]) == "--connbench" && i + 2 < argc) {
      const double sp = (i + 3 < argc) ? std::atof(argv[i + 3]) : 0.0;
      return x4sb::editor::runConnBench(std::atoi(argv[i + 1]), std::atoi(argv[i + 2]), sp);
    }
    if (std::string(argv[i]) == "--loadbench" && i + 1 < argc) {
      return x4sb::editor::runLoadBench(std::atoi(argv[i + 1]));
    }
    if (std::string(argv[i]) == "--rdc" && i + 1 < argc) {
      return x4sb::editor::runRdcCapture(std::string(argv[i + 1]));
    }
  }

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
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
    bool lodEnabled = false;  // editor default: show every module as a mesh (L toggles distance-LOD)
    std::string toast;
    double toastUntil = 0.0;

    while (!WindowShouldClose()) {
      {
        ZoneScopedN("input+update");
        cam.update();
        x4sb::editor::handleKeys(state);
        if (IsKeyPressed(KEY_G)) showGizmos = !showGizmos;
        if (IsKeyPressed(KEY_M)) showMeshes = !showMeshes;
        if (IsKeyPressed(KEY_L)) lodEnabled = !lodEnabled;  // distance-LOD box collapse for huge stations
        if (IsKeyPressed(KEY_F)) {
          ::Vector3 c{};
          float r = 20.0f;
          if (!focusSelection(state, c, r)) stationBounds(state, c, r);
          cam.frame(c, r);
        }
        const x4sb::editor::PlanIoOutcome io = x4sb::editor::handlePlanIoKeys(state);
        if (io.message) {
          toast = *io.message;
          toastUntil = GetTime() + 4.0;
        }
        if (io.reloaded) {
          // Editor model: upload the whole plan's meshes up front (loading screen)
          // rather than streaming the in-view set, so there's no box pop-in and
          // selecting/moving never hitches on a late upload (known-issues 1.3).
          x4sb::editor::loadStationMeshes(state.station(), state.catalog(), meshes);
        }

        x4sb::editor::handleMouse(state, cam.camera());
      }

      BeginDrawing();
      ClearBackground(::Color{30, 30, 38, 255});
      x4sb::editor::drawScene(state, cam.camera(), meshes, showGizmos, showMeshes, lodEnabled);
      x4sb::editor::drawHud(state, GetScreenWidth(), GetScreenHeight(), showGizmos);
      if (GetTime() < toastUntil) x4sb::editor::drawToast(toast, GetScreenHeight());
      EndDrawing();
      FrameMark;
    }
  }  // meshes destroyed here (UnloadModel) before CloseWindow

  CloseWindow();
  return 0;
}

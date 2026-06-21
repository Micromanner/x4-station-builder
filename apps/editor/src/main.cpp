// x4sb-editor — the interactive station builder (parent §4B unit 5). Thin raylib
// shell: load the catalog, then per frame map input into EditorState (render-free
// logic in libs/editorcore) and draw it. All scene geometry is in X4 native space;
// the renderer applies the single (1,1,-1) handedness flip.
#include "app_paths.hpp"
#include "clay.h"
#include "clay_raylib.hpp"
#include "connbench.hpp"
#include "gizmoshot.hpp"
#include "input.hpp"
#include "iprofile.hpp"
#include "loadbench.hpp"
#include "megashot.hpp"
#include "mesh_cache.hpp"
#include "mesh_load.hpp"
#include "orbit_camera.hpp"
#include "plan_io.hpp"
#include "profile.hpp"
#include "profiling.hpp"
#include "raylib.h"
#include "rdc_capture.hpp"
#include "render.hpp"
#include "snaptest.hpp"
#include "ui_fonts.hpp"
#include "ui_inspector.hpp"
#include "ui_palette.hpp"
#include "ui_topbar.hpp"
#include "uishot.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/editor_state.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    if (std::string(argv[i]) == "--uishot") {
      return x4sb::editor::runUiShot(std::string(argv[i + 1]));
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

    // Clay UI: fonts + arena live for the whole render loop. clayMem backs Clay's
    // arena and Clay keeps the raw pointer (no copy), so it must outlive every
    // Clay call below — hence a long-lived local in this block, not a temporary.
    x4sb::editor::UiFonts uiFonts = x4sb::editor::loadUiFonts();
    std::vector<std::uint8_t> clayMem =
        x4sb::editor::clayInit(GetScreenWidth(), GetScreenHeight(), uiFonts);
    // Free-place standoff baseline: captured from the orbit distance now and on each
    // build-mode entry (below), then held — so zooming never drags the ghost in/out.
    state.setPlaceDistance(cam.distance());
    bool showGizmos = true;
    bool showMeshes = true;  // render-only state; not in EditorState (that's render-free)
    bool lodEnabled =
        false;  // editor default: show every module as a mesh (L toggles distance-LOD)
    std::string toast;
    double toastUntil = 0.0;
    constexpr double kToastSeconds = 4.0;
    const auto setToast = [&](std::string message) {
      toast = std::move(message);
      toastUntil = GetTime() + kToastSeconds;
    };

    // The cart -> auto-layout run, shared by the Enter key and the Build button:
    // one undoable layout + a report toast + warming the new modules' meshes.
    const auto runBuild = [&]() {
      const x4sb::AutoLayoutReport rep = state.runAutoLayout();
      setToast("auto-layout: snapped " + std::to_string(rep.snapped) + ", floating " +
               std::to_string(rep.floating) + ", skipped " + std::to_string(rep.skipped));
      x4sb::editor::loadStationMeshes(state.station(), state.catalog(), meshes);
    };

    while (!WindowShouldClose()) {
      {
        ZoneScopedN("input+update");
        // Panel-aware UI capture: pointer over the top bar OR either side panel.
        // Computed before cam.update so the wheel can be routed (palette scroll vs
        // scene zoom) and a click over a panel doesn't also place/select in 3D.
        const float barH = x4sb::editor::topBarHeight();
        const float palW = x4sb::editor::paletteWidth();
        const float insW = x4sb::editor::inspectorWidth();
        const float mx = static_cast<float>(GetMouseX());
        const float my = static_cast<float>(GetMouseY());
        const float screenW = static_cast<float>(GetScreenWidth());
        const bool overBar = my < barH;
        const bool overPalette = !overBar && mx < palW;
        const bool overInspector = !overBar && mx >= screenW - insW;
        const bool pointerOnUi = overBar || overPalette || overInspector;

        // Zoom toward the scene point under the cursor (plain dolly over empty
        // space); the hit-test runs only on wheel frames. Over the palette the wheel
        // scrolls the list instead, so suppress camera zoom there.
        std::optional<x4sb::Vec3> zoomFocus;
        if (!overPalette && GetMouseWheelMove() != 0.0f)
          zoomFocus = x4sb::editor::zoomFocusUnderCursor(state, cam.camera());
        cam.update(zoomFocus, /*allowWheel=*/!overPalette);
        x4sb::editor::handleKeys(state);
        // Re-baseline the free-place standoff when Tab enters build mode, so a fresh
        // ghost sits at the current view depth (handleKeys already toggled the mode).
        if (IsKeyPressed(KEY_TAB) && state.placementEnabled())
          state.setPlaceDistance(cam.distance());
        if (IsKeyPressed(KEY_G)) showGizmos = !showGizmos;
        if (IsKeyPressed(KEY_M)) showMeshes = !showMeshes;
        if (IsKeyPressed(KEY_L))
          lodEnabled = !lodEnabled;  // distance-LOD box collapse for huge stations
        if (IsKeyPressed(KEY_F)) {
          ::Vector3 c{};
          float r = 20.0f;
          if (!focusSelection(state, c, r)) stationBounds(state, c, r);
          cam.frame(c, r);
        }
        const x4sb::editor::PlanIoOutcome io = x4sb::editor::handlePlanIoKeys(state);
        if (io.message) setToast(*io.message);
        if (io.reloaded) {
          // Editor model: upload the whole plan's meshes up front (loading screen)
          // rather than streaming the in-view set, so there's no box pop-in and
          // selecting/moving never hitches on a late upload (known-issues 1.3).
          x4sb::editor::loadStationMeshes(state.station(), state.catalog(), meshes);
        }

        if (IsKeyPressed(KEY_ENTER)) runBuild();

        // Feed Clay this frame's viewport + pointer, then suppress the 3D mouse when
        // the pointer is over any panel so a button/list click doesn't also place or
        // select in the scene.
        Clay_SetLayoutDimensions(Clay_Dimensions{static_cast<float>(GetScreenWidth()),
                                                 static_cast<float>(GetScreenHeight())});
        Clay_SetPointerState(Clay_Vector2{mx, my}, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        // Feed the wheel to Clay's scroll containers only when over the palette.
        const float wheel = overPalette ? GetMouseWheelMove() : 0.0f;
        Clay_UpdateScrollContainers(false, Clay_Vector2{0.0f, wheel * 30.0f}, GetFrameTime());
        if (!pointerOnUi) x4sb::editor::handleMouse(state, cam.camera());
      }

      BeginDrawing();
      ClearBackground(::Color{30, 30, 38, 255});
      x4sb::editor::drawScene(state, cam.camera(), meshes, showGizmos, showMeshes, lodEnabled);
      x4sb::editor::drawHud(state, GetScreenWidth(), GetScreenHeight(), showGizmos);
      // Chrome drawn over the HUD so it sits on top. Actions are dispatched right
      // after EndDrawing (mutating state for next frame is fine). The ChromeRoot and
      // the middle spacer carry no backgroundColor (alpha 0 -> no RECTANGLE emitted),
      // so the 3D scene shows through between the side panels.
      Clay_BeginLayout();
      x4sb::editor::TopBarAction action = x4sb::editor::TopBarAction::None;
      x4sb::editor::PaletteAction palAction;
      CLAY({.id = CLAY_ID("ChromeRoot"),
            .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                       .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        action = x4sb::editor::topBar(state, GetFPS());
        // Middle row: palette | transparent spacer (3D shows through) | inspector.
        CLAY(
            {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}}) {
          palAction = x4sb::editor::palette(state);
          CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}  // spacer
          x4sb::editor::inspector(state);
        }
      }
      const Clay_RenderCommandArray uiCmds = Clay_EndLayout();
      x4sb::editor::renderClayCommands(uiCmds, uiFonts);
      if (GetTime() < toastUntil) x4sb::editor::drawToast(toast, GetScreenHeight());
      EndDrawing();
      FrameMark;

      switch (action) {
        case x4sb::editor::TopBarAction::Save:
          setToast(x4sb::editor::savePlan(state));
          break;
        case x4sb::editor::TopBarAction::Open: {
          bool loaded = false;
          setToast(x4sb::editor::loadPlan(state, loaded));
          if (loaded) x4sb::editor::loadStationMeshes(state.station(), state.catalog(), meshes);
          break;
        }
        case x4sb::editor::TopBarAction::Undo:
          if (state.canUndo()) state.undo();
          break;
        case x4sb::editor::TopBarAction::Redo:
          if (state.canRedo()) state.redo();
          break;
        case x4sb::editor::TopBarAction::None:
          break;
      }

      switch (palAction.kind) {
        case x4sb::editor::PaletteActionKind::SetFilter:
          state.setFilter(palAction.category);
          break;
        case x4sb::editor::PaletteActionKind::ClearFilter:
          state.setFilter(std::nullopt);
          break;
        case x4sb::editor::PaletteActionKind::SetActive:
          state.setActiveIndex(palAction.index);
          break;
        case x4sb::editor::PaletteActionKind::ToggleAutoBuild:
          state.toggleAutoBuildMode();
          break;
        case x4sb::editor::PaletteActionKind::AdjustCart:
          state.cartAdjust(palAction.id, palAction.delta);
          break;
        case x4sb::editor::PaletteActionKind::Build:
          runBuild();
          break;
        case x4sb::editor::PaletteActionKind::None:
          break;
      }
    }

    x4sb::editor::unloadUiFonts(uiFonts);
  }  // meshes destroyed here (UnloadModel) before CloseWindow

  CloseWindow();
  return 0;
}

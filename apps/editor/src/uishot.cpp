#include "uishot.hpp"

#include "bloom.hpp"
#include "clay.h"
#include "clay_raylib.hpp"
#include "env_cubemap.hpp"
#include "gizmoshot.hpp"  // orbitCam
#include "mesh_cache.hpp"
#include "mesh_load.hpp"
#include "native_assets.hpp"
#include "raylib.h"
#include "raylib_convert.hpp"  // toRl
#include "render.hpp"
#include "sky.hpp"
#include "ui_fonts.hpp"
#include "ui_inspector.hpp"
#include "ui_palette.hpp"
#include "ui_topbar.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/editor_state.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace x4sb::editor {
namespace {
constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

// Render one frame (sky + scene + top bar) with the pointer parked at `mouse`, then
// shot. Draws the same Sky + sky-lighting the live editor loop uses, so these shots
// verify the real look, not the default-lit harness look.
void shoot(EditorState& state, const ::Camera3D& cam, MeshCache& meshes, Sky& sky,
           FramePipeline& pipeline, const UiFonts& fonts, ::Vector2 mouse,
           const std::string& outPath) {
  for (int i = 0; i < 3; ++i) {  // warm-up frames so the captured buffer is clean
    Clay_SetLayoutDimensions(Clay_Dimensions{static_cast<float>(GetScreenWidth()),
                                             static_cast<float>(GetScreenHeight())});
    Clay_SetPointerState(Clay_Vector2{mouse.x, mouse.y}, false);  // pointer present, not pressed
    BeginDrawing();
    pipeline.begin();
    ClearBackground(BLACK);
    sky.draw(cam);
    drawScene(state, cam, meshes, /*showGizmos=*/true, /*showMeshes=*/true,
              /*lodEnabled=*/false, sky.sun());
    pipeline.end();
    Clay_BeginLayout();
    CLAY({.id = CLAY_ID("ChromeRoot"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
      x4sb::editor::SkyPickerResult skyRes;
      (void)topBar(state, 60.0, x4sb::editor::SkyPickerState{}, skyRes);
      CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}}) {
        (void)palette(state);
        CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        inspector(state);
      }
    }
    const Clay_RenderCommandArray cmds = Clay_EndLayout();
    renderClayCommands(cmds, fonts);
    EndDrawing();
  }
  TakeScreenshot(outPath.c_str());
}
}  // namespace

int runUiShot(const NativeAssets& native, const std::string& outPrefix) {
  const ModuleCatalog* catalog = &native.catalog;
  InitWindow(kScreenW, kScreenH, "X4 Station Builder - uishot");
  SetTargetFPS(60);
  {
    MeshCache meshes{native};
    Sky sky{native};
    FramePipeline pipeline;
    EnvCubemap envCube{sky};
    meshes.setEnvMap(envCube.textureId(), envCube.maxLod());
    UiFonts fonts = loadUiFonts();
    std::vector<std::uint8_t> clayMem = clayInit(kScreenW, kScreenH, fonts);

    // A couple of modules so the bar sits over a real scene (id-sorted first two).
    EditorState state(*catalog);
    Station station;
    InstanceId id = 1;
    for (const auto& entry : catalog->all()) {
      PlacedModule pm;
      pm.instanceId = id++;
      pm.defId = entry.first;
      station.add(pm);
      if (id > 2) break;
    }
    state.loadStation(std::move(station));
    loadStationMeshes(state.station(), state.catalog(), meshes);

    const StationBounds b = stationBounds(state.station(), state.catalog());
    const double r = b.radius > 1.0 ? b.radius : 20.0;
    const ::Camera3D cam = orbitCam(toRl(flipZ(b.center)), r * 2.5, 0.6, 0.45);

    shoot(state, cam, meshes, sky, pipeline, fonts, ::Vector2{-10, -10},
          outPrefix + "_idle.png");  // pointer off-bar
    shoot(state, cam, meshes, sky, pipeline, fonts, ::Vector2{70, 18},
          outPrefix + "_hover.png");  // over Save
    shoot(state, cam, meshes, sky, pipeline, fonts, ::Vector2{120, 200},
          outPrefix + "_palette.png");  // pointer over a module row
    state.setAutoBuildMode(true);
    // Seed from the rendered view (sorted, player-buildable) so a VISIBLE top row
    // shows a non-zero count — catalog->all() is unordered and may hit hidden ids.
    for (const auto* def : state.filteredView()) {
      state.cartAdjust(def->id, 4);
      if (state.cartTotal() >= 8) break;
    }
    shoot(state, cam, meshes, sky, pipeline, fonts, ::Vector2{120, 200},
          outPrefix + "_autobuild.png");
    std::printf("uishot: wrote %s_{idle,hover,palette,autobuild}.png\n", outPrefix.c_str());

    unloadUiFonts(fonts);
  }
  CloseWindow();
  return 0;
}
}  // namespace x4sb::editor

#include "uishot.hpp"

#include "app_paths.hpp"
#include "clay.h"
#include "clay_raylib.hpp"
#include "gizmoshot.hpp"  // orbitCam
#include "mesh_cache.hpp"
#include "mesh_load.hpp"
#include "raylib.h"
#include "raylib_convert.hpp"  // toRl
#include "render.hpp"
#include "ui_fonts.hpp"
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

// Render one frame (scene + top bar) with the pointer parked at `mouse`, then shot.
void shoot(EditorState& state, const ::Camera3D& cam, MeshCache& meshes, const UiFonts& fonts,
           ::Vector2 mouse, const std::string& outPath) {
  for (int i = 0; i < 3; ++i) {  // warm-up frames so the captured buffer is clean
    Clay_SetLayoutDimensions(Clay_Dimensions{static_cast<float>(GetScreenWidth()),
                                             static_cast<float>(GetScreenHeight())});
    Clay_SetPointerState(Clay_Vector2{mouse.x, mouse.y}, false);  // pointer present, not pressed
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    drawScene(state, cam, meshes, /*showGizmos=*/true, /*showMeshes=*/true);
    Clay_BeginLayout();
    (void)topBar(state, 60.0);
    const Clay_RenderCommandArray cmds = Clay_EndLayout();
    renderClayCommands(cmds, fonts);
    EndDrawing();
  }
  TakeScreenshot(outPath.c_str());
}
}  // namespace

int runUiShot(const std::string& outPrefix) {
  const std::optional<std::string> catalogPath = findCatalogJson();
  if (!catalogPath) {
    std::fprintf(stderr, "uishot: no catalog.json\n");
    return 1;
  }
  std::optional<ModuleCatalog> catalog = ModuleCatalog::loadFromFile(*catalogPath);
  if (!catalog) {
    std::fprintf(stderr, "uishot: failed to load catalog\n");
    return 1;
  }
  InitWindow(kScreenW, kScreenH, "X4 Station Builder - uishot");
  SetTargetFPS(60);
  {
    const std::filesystem::path assetRoot = std::filesystem::path(*catalogPath).parent_path();
    MeshCache meshes{assetRoot};
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

    shoot(state, cam, meshes, fonts, ::Vector2{-10, -10},
          outPrefix + "_idle.png");  // pointer off-bar
    shoot(state, cam, meshes, fonts, ::Vector2{70, 18}, outPrefix + "_hover.png");  // over Save
    std::printf("uishot: wrote %s_{idle,hover}.png\n", outPrefix.c_str());

    unloadUiFonts(fonts);
  }
  CloseWindow();
  return 0;
}
}  // namespace x4sb::editor

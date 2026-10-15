// x4sb-editor — the interactive station builder (parent §4B unit 5). Thin raylib
// shell: load the catalog, then per frame map input into EditorState (render-free
// logic in libs/editorcore) and draw it. All scene geometry is in X4 native space;
// the renderer applies the single (1,1,-1) handedness flip.
#include "bloom.hpp"
#include "clay.h"
#include "clay_raylib.hpp"
#include "connbench.hpp"
#include "env_cubemap.hpp"
#include "envdump.hpp"
#include "external/glad.h"
#include "gizmoshot.hpp"
#include "gl_ext.hpp"
#include "graphics_settings.hpp"
#include "input.hpp"
#include "iprofile.hpp"
#include "loadbench.hpp"
#include "megashot.hpp"
#include "mesh_cache.hpp"
#include "mesh_load.hpp"
#include "native_assets.hpp"
#include "orbit_camera.hpp"
#include "plan_io.hpp"
#include "profile.hpp"
#include "profiling.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rdc_capture.hpp"
#include "render.hpp"
#include "sky.hpp"
#include "snaptest.hpp"
#include "sun_shadow.hpp"
#include "sunshot.hpp"
#include "ui_fonts.hpp"
#include "ui_inspector.hpp"
#include "ui_palette.hpp"
#include "ui_suntuning.hpp"
#include "ui_topbar.hpp"
#include "uishot.hpp"
#include "viewshot.hpp"
#include "x4sb/assetpipe/native_background.hpp"
#include "x4sb/assetpipe/sectorindex.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/editor_state.hpp"
#include "x4sb/planio/plan.hpp"
#include "x4sb/platform/platform.hpp"
#include "x4sb/snap/pick.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
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

// Free VRAM in MiB, -1 when the driver exposes neither query. GL_ATI_meminfo (AMD)
// and GL_NVX_gpu_memory_info (NVIDIA) are not in raylib's glad, hence the raw enums;
// an unsupported enum only raises GL_INVALID_ENUM, which is drained here.
[[nodiscard]] int freeVramMiB() {
  constexpr GLenum kTextureFreeMemoryAti = 0x87FC;
  constexpr GLenum kCurrentAvailableVidmemNvx = 0x9049;
  while (glGetError() != GL_NO_ERROR) {
  }
  for (const GLenum query : {kTextureFreeMemoryAti, kCurrentAvailableVidmemNvx}) {
    std::array<GLint, 4> kib{-1, -1, -1, -1};
    glGetIntegerv(query, kib.data());
    if (glGetError() == GL_NO_ERROR && kib[0] >= 0) return kib[0] / 1024;
  }
  return -1;
}

// Local wall-clock time, so a logged spike can be matched to a build and to the
// moment the user noticed the freeze.
[[nodiscard]] std::string wallClock() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::array<char, 32> text{};
  const std::size_t n = std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S", &local);
  return {text.data(), n};
}

}  // namespace

int main(int argc, char** argv) {
  std::string gameDirectory;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--x4") gameDirectory = argv[i + 1];
  // Harness modes load the game metadata up front (no window needed) and fail fast.
  const auto harness = [&gameDirectory](auto&& run) -> int {
    const auto native = x4sb::editor::loadHarnessAssets(gameDirectory);
    return native ? run(*native) : 1;
  };
  using x4sb::editor::NativeAssets;
  // Hidden visual-regression harness: `--snaptest <outPrefix>` renders a snapped
  // pair, screenshots it, and exits (no interactive window). It owns its own
  // InitWindow/CloseWindow, so dispatch BEFORE the interactive InitWindow below.
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--snaptest") {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runSnapTest(native, std::string(argv[i + 1]));
      });
    }
    if (std::string(argv[i]) == "--megashot" && i + 2 < argc) {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runMegaShot(native, std::string(argv[i + 1]),
                                         std::string(argv[i + 2]));
      });
    }
    if (std::string(argv[i]) == "--viewshot" && i + 3 < argc) {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runViewShot(native, std::string(argv[i + 1]), std::string(argv[i + 2]),
                                         std::string(argv[i + 3]));
      });
    }
    if (std::string(argv[i]) == "--gizmoshot") {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runGizmoShot(native, std::string(argv[i + 1]));
      });
    }
    if (std::string(argv[i]) == "--uishot") {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runUiShot(native, std::string(argv[i + 1]));
      });
    }
    if (std::string(argv[i]) == "--envdump") {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runEnvDump(native, std::string(argv[i + 1]));
      });
    }
    if (std::string(argv[i]) == "--sunshot" && i + 2 < argc) {
      // Trailing key=value options: exposure, w, h, fovy, ndcx, ndcy,
      // half=both|glow|flares|none (defaults = the live editor, sun centred).
      x4sb::editor::SunShotOptions opt;
      for (int j = i + 3; j < argc; ++j) {
        const std::string kv = argv[j];
        const std::size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = kv.substr(0, eq);
        const std::string val = kv.substr(eq + 1);
        const float f = std::strtof(val.c_str(), nullptr);
        if (key == "exposure" && f > 0.0f) opt.sceneExposure = f;
        if (key == "w" && f >= 64.0f) opt.width = static_cast<int>(f);
        if (key == "h" && f >= 64.0f) opt.height = static_cast<int>(f);
        if (key == "fovy" && f > 1.0f && f < 179.0f) opt.fovy = f;
        if (key == "ndcx") opt.sunNdcX = f;
        if (key == "ndcy") opt.sunNdcY = f;
        if (key == "half") {
          opt.drawGlow = val == "both" || val == "glow";
          opt.drawFlares = val == "both" || val == "flares";
        }
      }
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runSunShot(native, std::string(argv[i + 1]), std::string(argv[i + 2]),
                                        opt);
      });
    }
    if (std::string(argv[i]) == "--gizmosweep") {
      return harness(
          [&](const NativeAssets& native) { return x4sb::editor::runGizmoSweep(native); });
    }
    if (std::string(argv[i]) == "--profile" && i + 2 < argc) {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runProfile(native, std::string(argv[i + 1]), std::atoi(argv[i + 2]));
      });
    }
    if (std::string(argv[i]) == "--iprofile" && i + 2 < argc) {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runInteractiveProfile(native, std::string(argv[i + 1]),
                                                   std::atoi(argv[i + 2]));
      });
    }
    if (std::string(argv[i]) == "--connbench" && i + 2 < argc) {
      const double sp = (i + 3 < argc) ? std::atof(argv[i + 3]) : 0.0;
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runConnBench(native, std::atoi(argv[i + 1]), std::atoi(argv[i + 2]),
                                          sp);
      });
    }
    if (std::string(argv[i]) == "--loadbench" && i + 1 < argc) {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runLoadBench(native, std::atoi(argv[i + 1]));
      });
    }
    if (std::string(argv[i]) == "--rdc" && i + 1 < argc) {
      return harness([&](const NativeAssets& native) {
        return x4sb::editor::runRdcCapture(native, std::string(argv[i + 1]));
      });
    }
  }

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(kScreenW, kScreenH, "X4 Station Builder");
  SetTargetFPS(60);
  if (!x4sb::editor::requireGlExtensions()) {
    CloseWindow();
    return 1;
  }

  std::optional<NativeAssets> native;
  if (gameDirectory.empty())
    gameDirectory = x4sb::platform::loadGameDirectory(x4sb::platform::settingsFilePath());
  if (!gameDirectory.empty()) native = x4sb::editor::loadNativeAssets(gameDirectory);
  while (!native) {
    const auto selected = x4sb::editor::chooseGameDirectory(gameDirectory, !gameDirectory.empty());
    if (!selected) {
      CloseWindow();
      return 0;
    }
    gameDirectory = *selected;
    native = x4sb::editor::loadNativeAssets(gameDirectory);
  }
  std::error_code pathError;
  const auto absolute = std::filesystem::canonical(gameDirectory, pathError);
  if (!pathError) gameDirectory = absolute.string();
  if (!x4sb::platform::saveGameDirectory(x4sb::platform::settingsFilePath(), gameDirectory))
    TraceLog(LOG_WARNING, "Could not save the X4 install location");

  x4sb::EditorState state(native->catalog);

  // MeshCache owns raylib Models, so it must be constructed after InitWindow (GL
  // context required by LoadModel) and destroyed before CloseWindow (UnloadModel
  // also needs the context). It and the render loop live in this inner block so
  // the cache's destructor runs at the block's close — before the explicit
  // CloseWindow() below.
  {
    x4sb::editor::MeshCache meshes{*native};
    x4sb::editor::OrbitCamera cam;
    ::Camera3D lastCamera = cam.camera();  // for the starfield's camera-motion dim

    // Sky picker inputs: sector index + the persisted global setting.
    const std::vector<x4sb::assetpipe::SectorIndexEntry> sectorIndex =
        x4sb::assetpipe::readSectorIndex(x4sb::assetpipe::nativeSectorIndex(*native->archive));
    std::string skySetting = x4sb::platform::loadSkySetting(x4sb::platform::settingsFilePath());
    const auto skyAvailable = [&native](const std::string& cluster) {
      return x4sb::assetpipe::clusterComponentPath(*native->archive, cluster).has_value();
    };
    const auto randomSector = [&]() -> const x4sb::assetpipe::SectorIndexEntry* {
      std::vector<const x4sb::assetpipe::SectorIndexEntry*> available;
      for (const auto& e : sectorIndex)
        if (skyAvailable(e.cluster)) available.push_back(&e);
      if (available.empty()) return nullptr;
      std::mt19937 rng{std::random_device{}()};
      std::uniform_int_distribution<std::size_t> pick{0, available.size() - 1};
      return available[pick(rng)];
    };
    const x4sb::assetpipe::SectorIndexEntry* startSector = nullptr;
    if (skySetting != "random") {
      for (const auto& e : sectorIndex)
        if (e.macro == skySetting && skyAvailable(e.cluster)) {
          startSector = &e;
          break;
        }
      if (startSector == nullptr)
        TraceLog(LOG_WARNING, "Sky: setting '%s' unavailable; falling back to random",
                 skySetting.c_str());
    }
    if (startSector == nullptr) startSector = randomSector();
    // No sector index -> Sky's cluster_11 default.
    x4sb::editor::Sky sky{*native, startSector != nullptr ? startSector->cluster : std::string{},
                          startSector != nullptr ? startSector->macro : std::string{}};
    x4sb::editor::FramePipeline pipeline;
    x4sb::editor::EnvCubemap envCube{sky};
    meshes.setEnvMap(envCube.textureId(), envCube.maxLod());
    x4sb::editor::SunShadow sunShadow;

    // Frame tuning (F10 panel): the pre-tonemap exposure, 1.0 = X4's units.
    bool sunTuneOpen = false;
    float sceneExposure = 1.0f;

    // Sky picker UI state: dropdown open flag + the current control label
    // (derived from the persisted setting, matching sky's startup resolution above).
    bool skyOpen = false;
    std::string skyLabel = "Sky: Random";
    if (skySetting != "random")
      for (const auto& e : sectorIndex)
        if (e.macro == skySetting) {
          skyLabel = "Sky: " + e.name;
          break;
        }

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
    x4sb::editor::GraphicsSettings gfx;
    x4sb::editor::applyGraphicsSettings(gfx, meshes, sky);
    double animTime = 0.0;
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
      const char* modeName =
          state.layoutMode() == x4sb::LayoutMode::Organic ? "Organic" : "Fishbone";
      std::string label = std::string(modeName) + ": snapped " + std::to_string(rep.snapped) +
                          ", floating " + std::to_string(rep.floating) + ", skipped " +
                          std::to_string(rep.skipped);
      if (rep.connectorsAdded > 0) label += ", +" + std::to_string(rep.connectorsAdded) + " conn";
      setToast(label);
      x4sb::editor::loadStationMeshes(state.station(), state.catalog(), meshes);
    };

    // Frame-phase spike log: a frame over kSpikeMs appends one line with the
    // per-phase split (ms) to x4sb_spikes.log in the working dir, so a freeze the
    // user sees can be attributed to a phase without a profiler build attached.
    constexpr double kSpikeMs = 100.0;
    std::ofstream("x4sb_spikes.log", std::ios::app)
        << "session " << wallClock() << "  freeVram=" << freeVramMiB() << "MiB\n";
    std::array<double, 7> phaseT{};
    const auto stamp = [&](std::size_t k) { phaseT[k] = GetTime(); };

    while (!WindowShouldClose()) {
      stamp(0);
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
        const bool pointerOnUi = overBar || overPalette || overInspector || skyOpen || sunTuneOpen;

        // Zoom toward the scene point under the cursor (plain dolly over empty
        // space); the hit-test runs only on wheel frames. Over the palette or an
        // open sky dropdown the wheel scrolls the list instead, so suppress camera
        // zoom there.
        std::optional<x4sb::Vec3> zoomFocus;
        if (!overPalette && !skyOpen && GetMouseWheelMove() != 0.0f)
          zoomFocus = x4sb::editor::zoomFocusUnderCursor(state, cam.camera());
        cam.update(zoomFocus, /*allowWheel=*/!overPalette && !skyOpen);
        x4sb::editor::handleKeys(state);
        // Re-baseline the free-place standoff when Tab enters build mode, so a fresh
        // ghost sits at the current view depth (handleKeys already toggled the mode).
        if (IsKeyPressed(KEY_TAB) && state.placementEnabled())
          state.setPlaceDistance(cam.distance());
        if (IsKeyPressed(KEY_F10)) sunTuneOpen = !sunTuneOpen;
        if (IsKeyPressed(KEY_G)) showGizmos = !showGizmos;
        if (IsKeyPressed(KEY_M)) showMeshes = !showMeshes;
        if (IsKeyPressed(KEY_F12)) {
          pipeline.requestDump("livedump");
          TakeScreenshot("livedump_screen.png");
          // Plus everything `--viewshot` needs to re-render this exact view headless.
          {
            const ::Camera3D& c = cam.camera();
            nlohmann::json j;
            j["position"] = {c.position.x, c.position.y, c.position.z};
            j["target"] = {c.target.x, c.target.y, c.target.z};
            j["up"] = {c.up.x, c.up.y, c.up.z};
            j["fovy"] = c.fovy;
            j["width"] = GetScreenWidth();
            j["height"] = GetScreenHeight();
            j["cluster"] = sky.clusterDir();
            j["sector"] = sky.sectorMacro();
            j["exposure"] = sceneExposure;
            std::ofstream(std::string("livedump_view.json"), std::ios::binary) << j.dump(1);
            std::ofstream(std::string("livedump_plan.xml"), std::ios::binary)
                << x4sb::exportPlanXml(state.station(), state.catalog());
          }
          TraceLog(LOG_INFO,
                   "Frame dump requested: livedump_*.png/.raw + livedump_view.json + "
                   "livedump_plan.xml in the working directory");
        }
        // F1: trace the LOD decisions of the module under the cursor (not selected: a
        // selected module draws at full detail); F2: stop, write lodtrace.txt.
        if (IsKeyPressed(KEY_F1)) {
          x4sb::Vec3 ro{};
          x4sb::Vec3 rd{};
          x4sb::editor::mouseRayX4(cam.camera(), ro, rd);
          if (const auto hit = x4sb::pickModule(state.station(), state.catalog(), ro, rd)) {
            x4sb::editor::startLodTrace(*hit);
            TraceLog(LOG_INFO, "LOD trace started (F2 stops)");
          }
        }
        if (IsKeyPressed(KEY_F2)) {
          std::ofstream("lodtrace.txt", std::ios::binary) << x4sb::editor::stopLodTrace();
          TraceLog(LOG_INFO, "LOD trace written to lodtrace.txt");
        }
        if (IsKeyPressed(KEY_K)) {
          gfx.sunFx = !gfx.sunFx;
          x4sb::editor::applyGraphicsSettings(gfx, meshes, sky);
          TraceLog(LOG_INFO, "Sun lens effect: %s", gfx.sunFx ? "on" : "off");
        }
        if (IsKeyPressed(KEY_L)) gfx.distanceLod = !gfx.distanceLod;
        if (IsKeyPressed(KEY_P)) {
          gfx.animation = !gfx.animation;
          TraceLog(LOG_INFO, "Animation: %s", gfx.animation ? "on" : "paused");
        }
        if (gfx.animation) animTime += static_cast<double>(GetFrameTime());
        sky.setAnimationTime(animTime);
        if (const float dt = GetFrameTime(); dt > 0.0f) {
          const ::Camera3D& c = cam.camera();
          const auto forward = [](const ::Camera3D& v) {
            return Vector3Normalize(Vector3Subtract(v.target, v.position));
          };
          sky.setCameraMotion(Vector3Distance(c.position, lastCamera.position) / dt,
                              Vector3Angle(forward(c), forward(lastCamera)) / dt);
          lastCamera = c;
        }
        meshes.setAnimationTime(animTime);
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
        const float wheel = (overPalette || skyOpen) ? GetMouseWheelMove() : 0.0f;
        Clay_UpdateScrollContainers(false, Clay_Vector2{0.0f, wheel * 30.0f}, GetFrameTime());
        if (!pointerOnUi) x4sb::editor::handleMouse(state, cam.camera());
      }

      stamp(1);
      BeginDrawing();
      // Shadow depth pass first: it owns its own FBO and must not nest inside the
      // pipeline's scene target.
      if (gfx.shadows)
        sunShadow.render(state.station(), state.catalog(), meshes, sky.sun().dir, cam.camera());
      else
        meshes.setShadowCascades(0);
      stamp(2);
      pipeline.begin();
      ClearBackground(BLACK);

      BeginMode3D(cam.camera());
      sky.draw(cam.camera());
      EndMode3D();

      x4sb::editor::drawScene(state, cam.camera(), meshes, showGizmos, showMeshes, gfx.distanceLod,
                              sky.sun(), &sky.fog());
      stamp(3);
      pipeline.end([&] {
        if (gfx.sunFx) sky.drawLensFlares(cam.camera(), GetScreenWidth(), GetScreenHeight());
      });
      stamp(4);
      x4sb::editor::drawHud(state, GetScreenWidth(), GetScreenHeight(), showGizmos);
      // Chrome drawn over the HUD so it sits on top. Actions are dispatched right
      // after EndDrawing (mutating state for next frame is fine). The ChromeRoot and
      // the middle spacer carry no backgroundColor (alpha 0 -> no RECTANGLE emitted),
      // so the 3D scene shows through between the side panels.
      Clay_BeginLayout();
      x4sb::editor::TopBarAction action = x4sb::editor::TopBarAction::None;
      x4sb::editor::PaletteAction palAction;
      x4sb::editor::SkyPickerResult skyRes;
      x4sb::editor::SunTuneResult sunTuneRes;
      CLAY({.id = CLAY_ID("ChromeRoot"),
            .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                       .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        action = x4sb::editor::topBar(
            state, GetFPS(), x4sb::editor::SkyPickerState{&sectorIndex, skyLabel.c_str(), skyOpen},
            skyRes);
        x4sb::editor::sunTuningPanel(x4sb::editor::SunTuneState{sunTuneOpen, sceneExposure},
                                     sunTuneRes);
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
      stamp(5);
      EndDrawing();
      stamp(6);
      FrameMark;
      if ((phaseT[6] - phaseT[0]) * 1000.0 > kSpikeMs) {
        const auto ms = [&](std::size_t a, std::size_t b) {
          return (phaseT[b] - phaseT[a]) * 1000.0;
        };
        const ::Vector3 cp = cam.camera().position;
        const ::Vector3 ct = cam.camera().target;
        const auto stats = meshes.statistics();
        std::ofstream("x4sb_spikes.log", std::ios::app)
            << wallClock() << " spike " << ms(0, 6) << "ms  update=" << ms(0, 1)
            << " shadow=" << ms(1, 2) << " scene=" << ms(2, 3) << " post=" << ms(3, 4)
            << " ui=" << ms(4, 5) << " present=" << ms(5, 6) << "  cam=(" << cp.x << "," << cp.y
            << "," << cp.z << ") target=(" << ct.x << "," << ct.y << "," << ct.z
            << ") modules=" << state.station().modules().size() << " models=" << stats.loadedModels
            << " pending=" << stats.pendingModels << " textures=" << stats.loadedTextures
            << " freeVram=" << freeVramMiB() << "MiB\n";
      }

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
        case x4sb::editor::TopBarAction::Shuffle: {
          const x4sb::AutoLayoutReport rep = state.runAutoLayoutShuffle();
          std::string label = "shuffle: snapped " + std::to_string(rep.snapped) + ", floating " +
                              std::to_string(rep.floating) + ", skipped " +
                              std::to_string(rep.skipped);
          if (rep.connectorsAdded > 0)
            label += ", +" + std::to_string(rep.connectorsAdded) + " conn";
          setToast(label);
          x4sb::editor::loadStationMeshes(state.station(), state.catalog(), meshes);
          break;
        }
        case x4sb::editor::TopBarAction::None:
          break;
      }

      if (!palAction.hoveredId.empty())
        if (const x4sb::ModuleDef* def = state.catalog().find(palAction.hoveredId))
          for (const x4sb::MeshRef& ref : def->meshRefs)
            meshes.request(ref.meshKey, /*optional=*/false, /*urgent=*/true);

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
        case x4sb::editor::PaletteActionKind::SetFaction:
          state.setFaction(palAction.faction);
          break;
        case x4sb::editor::PaletteActionKind::CycleMode:
          state.cycleLayoutMode();
          break;
        case x4sb::editor::PaletteActionKind::None:
          break;
      }

      // Frame-tuning dispatch: step, clamp, push to the pipeline, log so the
      // winning value can be hardcoded afterwards.
      if (sunTuneRes.sceneExpDelta != 0) {
        sceneExposure = std::clamp(
            sceneExposure + 0.05f * static_cast<float>(sunTuneRes.sceneExpDelta), 0.05f, 4.0f);
        pipeline.setSceneExposure(sceneExposure);
        TraceLog(LOG_INFO, "FrameTuning: sceneExposure=%.2f", static_cast<double>(sceneExposure));
      }

      // Sky picker dispatch. Reassigning skyLabel here is safe: Clay commands were
      // already rendered before EndDrawing above, and next frame re-reads it.
      // sky.reload + envCube.rebake run here (post-EndDrawing, outside FramePipeline's
      // begin/end), satisfying the rebake frame-phase contract.
      // No raylib input reads here: EndDrawing's PollInputEvents already advanced
      // the frame's input state, so a release checked post-dispatch fires a frame
      // early and eats row clicks. All click decisions come from the layout phase.
      if (skyRes.toggleOpen) {
        skyOpen = !skyOpen;
      } else if (skyRes.clickAway) {
        skyOpen = false;
      }
      if (skyRes.picked != x4sb::editor::kSkyPickNone) {
        skyOpen = false;
        const x4sb::assetpipe::SectorIndexEntry* target = nullptr;
        std::string name = "Random";
        std::string settingValue = "random";
        if (skyRes.picked >= 0 && static_cast<std::size_t>(skyRes.picked) < sectorIndex.size()) {
          const auto& e = sectorIndex[static_cast<std::size_t>(skyRes.picked)];
          if (!skyAvailable(e.cluster)) {
            setToast("Sky unavailable: " + e.name + " (no background in the game files)");
            settingValue.clear();  // sentinel: don't persist a dead pick
          } else {
            target = &e;
            name = e.name;
            settingValue = e.macro;
          }
        } else {
          target = randomSector();
        }
        if (!settingValue.empty()) {
          x4sb::platform::saveSkySetting(x4sb::platform::settingsFilePath(), settingValue);
          skyLabel = "Sky: " + name;
        }
        if (target != nullptr) {
          sky.reload(target->cluster, target->macro);
          envCube.rebake(sky);
          setToast("Sky: " + name);
        }
      }
    }

    x4sb::editor::unloadUiFonts(uiFonts);
  }  // meshes destroyed here (UnloadModel) before CloseWindow

  CloseWindow();
  return 0;
}

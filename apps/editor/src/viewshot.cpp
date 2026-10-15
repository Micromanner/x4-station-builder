#include "viewshot.hpp"

#include "bloom.hpp"
#include "env_cubemap.hpp"
#include "mesh_cache.hpp"
#include "mesh_load.hpp"
#include "native_assets.hpp"
#include "raylib.h"
#include "render.hpp"
#include "sky.hpp"
#include "sun_shadow.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/editorcore/mesh_paths.hpp"
#include "x4sb/planio/plan.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace x4sb::editor {
namespace {

struct View {
  ::Camera3D camera{};
  int width = 1280;
  int height = 720;
  std::string cluster;
  std::string sector;
  float exposure = 1.0f;
  double time = 0.0;  // animation clock, seconds: still frame by default
  bool pom = true;
  int normalBinormalSign = -1;
  // The station's position in the cluster (X4 axes, metres) for the fog; unset =
  // the sector origin. Capture A/B: from the capture's registration.
  std::optional<Vec3> stationPos;
};

::Vector3 vec3Of(const nlohmann::json& a) {
  return ::Vector3{a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>()};
}

std::optional<View> readView(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  try {
    const nlohmann::json j = nlohmann::json::parse(in);
    View v;
    v.camera.position = vec3Of(j.at("position"));
    v.camera.target = vec3Of(j.at("target"));
    v.camera.up = vec3Of(j.at("up"));
    v.camera.fovy = j.at("fovy").get<float>();
    v.camera.projection = CAMERA_PERSPECTIVE;
    v.width = j.at("width").get<int>();
    v.height = j.at("height").get<int>();
    v.cluster = j.value("cluster", std::string{});
    v.sector = j.value("sector", std::string{});
    v.exposure = j.value("exposure", 1.0f);
    v.time = j.value("time", 0.0);
    v.pom = j.value("pom", true);
    v.normalBinormalSign = j.value("normalBinormalSign", -1);
    if (v.normalBinormalSign != -1 && v.normalBinormalSign != 1) return std::nullopt;
    if (j.contains("stationPos")) {
      const auto& s = j.at("stationPos");
      const Vec3 pos{s.at(0).get<double>(), s.at(1).get<double>(), s.at(2).get<double>()};
      if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
        return std::nullopt;
      v.stationPos = pos;
    }
    if (v.width < 64 || v.height < 64 || v.camera.fovy <= 0.0f) return std::nullopt;
    return v;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

void renderFrame(const Station& station, const ModuleCatalog& catalog, const View& view,
                 MeshCache& meshes, Sky& sky, FramePipeline& pipeline, SunShadow& shadow) {
  BeginDrawing();
  shadow.render(station, catalog, meshes, sky.sun().dir, view.camera);
  pipeline.begin();
  ClearBackground(BLACK);
  BeginMode3D(view.camera);
  sky.draw(view.camera);
  EndMode3D();
  // Editor default: LOD off (every module as a mesh), gizmos off so the dump is
  // pure scene.
  drawScene(station, catalog, view.camera, meshes, /*showGizmos=*/false, /*showMeshes=*/true,
            /*allConnectors=*/false, /*lodEnabled=*/false, sky.sun(), &sky.fog());
  pipeline.end();
  EndDrawing();
}

}  // namespace

int runViewShot(const NativeAssets& native, const std::string& planPath,
                const std::string& viewJsonPath, const std::string& outPrefix) {
  const ModuleCatalog* catalog = &native.catalog;
  std::ifstream in(planPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "viewshot: cannot open plan %s\n", planPath.c_str());
    return 1;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::optional<Station> station = importPlanXml(ss.str());
  if (!station) {
    std::fprintf(stderr, "viewshot: failed to parse plan %s\n", planPath.c_str());
    return 1;
  }
  const std::optional<View> view = readView(viewJsonPath);
  if (!view) {
    std::fprintf(stderr, "viewshot: bad view json %s\n", viewJsonPath.c_str());
    return 1;
  }

  SetConfigFlags(FLAG_WINDOW_HIDDEN);
  InitWindow(view->width, view->height, "X4 Station Builder - viewshot");
  bool failed = false;
  {
    MeshCache meshes{native};
    Sky sky{native, view->cluster, view->sector};
    sky.setAnimationTime(view->time);
    if (view->stationPos) sky.fog().setOrigin(*view->stationPos);
    meshes.setAnimationTime(view->time);
    meshes.setPomEnabled(view->pom);
    meshes.setNormalBinormalSign(view->normalBinormalSign);
    FramePipeline pipeline;
    pipeline.setSceneExposure(view->exposure);
    EnvCubemap envCube{sky};
    meshes.setEnvMap(envCube.textureId(), envCube.maxLod());
    SunShadow shadow;

    // Same pre-upload as the editor's loading screen: the per-frame streaming
    // budget would leave a big station as placeholder boxes for hundreds of frames.
    const auto loadStart = std::chrono::steady_clock::now();
    loadStationMeshes(*station, *catalog, meshes);
    const double loadSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStart).count();
    const auto stats = meshes.statistics();
    std::size_t requiredMissing = 0;
    for (const auto& path : meshPathsFor(*station, *catalog))
      if (meshes.warm(path) == nullptr) ++requiredMissing;
    failed =
        requiredMissing != 0 || !stats.shadersReady || !sky.isValid() || sky.missingAssets() != 0;
    std::ofstream report(outPrefix + "_streaming.json");
    report << nlohmann::json{{"metadataCacheHit", native.metadataCacheHit},
                             {"metadataSeconds", native.metadataSeconds},
                             {"meshLoadSeconds", loadSeconds},
                             {"loadedModels", stats.loadedModels},
                             {"requiredMeshesMissing", requiredMissing},
                             {"missingModelsIncludingOptionalLods", stats.missingModels},
                             {"loadedTextures", stats.loadedTextures},
                             {"missingTextures", stats.missingTextures},
                             {"meshRanges", stats.meshRanges},
                             {"uploadedVertexBytes", stats.uploadedVertexBytes},
                             {"uploadedIndexBytes", stats.uploadedIndexBytes},
                             {"partVertices", stats.partVertices},
                             {"partIndices", stats.partIndices},
                             {"textureBytes", stats.textureBytes},
                             {"materials", stats.materials},
                             {"litOpaqueRanges", stats.litOpaqueRanges},
                             {"litMaskRanges", stats.litMaskRanges},
                             {"unlitOpaqueRanges", stats.unlitOpaqueRanges},
                             {"transparentRanges", stats.transparentRanges},
                             {"litOpaqueMaterials", stats.litOpaqueMaterials},
                             {"litMaskMaterials", stats.litMaskMaterials},
                             {"maxRangeIndices", stats.maxRangeIndices},
                             {"shadersReady", stats.shadersReady},
                             {"skyValid", sky.isValid()},
                             {"skyMissingAssets", sky.missingAssets()},
                             {"skyDomes", sky.domeCount()},
                             {"sunSprites", sky.sunSpriteCount()},
                             {"fogMedia", sky.fog().mediumCount()},
                             {"fogSpheres", sky.fog().sphereCount()}}
                  .dump(2);
    for (int i = 0; i < 3; ++i)
      renderFrame(*station, *catalog, *view, meshes, sky, pipeline, shadow);

    // Full frame first, then each term alone (mask bit0 sun diffuse, bit1 sun
    // specular, bit2 IBL, bit3 local lights; 0 = emissive + fill only; 16 = the
    // shadow term: sun visibility, camera distance / 10 km, n.l in r/g/b; 32 =
    // albedo, 64 = roughness/metallic in r/g, 128 = SSAO).
    struct Term {
      int mask;
      const char* label;
    };
    constexpr std::array<Term, 11> kTerms{{{15, "all"},
                                           {1, "sundiffuse"},
                                           {2, "sunspecular"},
                                           {4, "ibl"},
                                           {8, "local"},
                                           {0, "emissive"},
                                           {16, "shadow"},
                                           {32, "albedo"},
                                           {64, "metalrough"},
                                           {128, "ao"},
                                           {256, "normal"}}};
    for (const Term& t : kTerms) {
      meshes.setLightMask(t.mask);
      // Lighting terms isolate the hull: only the full frame carries the fog.
      sky.fog().setEnabled(t.mask == 15);
      renderFrame(*station, *catalog, *view, meshes, sky, pipeline, shadow);
      pipeline.requestDump(outPrefix + "_" + t.label);
      renderFrame(*station, *catalog, *view, meshes, sky, pipeline, shadow);
      std::printf("viewshot: %s dumped\n", t.label);
    }
    meshes.setLightMask(15);
  }
  CloseWindow();
  return failed ? 1 : 0;
}

}  // namespace x4sb::editor

#include "sunshot.hpp"

#include "bloom.hpp"
#include "raylib.h"
#include "raymath.h"
#include "sky.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace x4sb::editor {

namespace {
// Camera basis that puts `sunDir` at the requested NDC: the minimal rotation
// taking the view-space sun vector onto the world sun direction.
::Camera3D aimCamera(const ::Vector3& sunDir, const SunShotOptions& o) {
  const float tanHalf = std::tan(0.5f * o.fovy * DEG2RAD);
  const float aspect = static_cast<float>(o.width) / static_cast<float>(o.height);
  // raylib's right = cross(forward, up) = -x of this frame, hence the negated x.
  const ::Vector3 v =
      Vector3Normalize(::Vector3{-o.sunNdcX * tanHalf * aspect, o.sunNdcY * tanHalf, 1.0f});
  const ::Quaternion q = QuaternionFromVector3ToVector3(v, sunDir);
  ::Camera3D cam{};
  cam.position = ::Vector3{0.0f, 0.0f, 0.0f};
  cam.target = Vector3RotateByQuaternion(::Vector3{0.0f, 0.0f, 1.0f}, q);
  cam.up = Vector3RotateByQuaternion(::Vector3{0.0f, 1.0f, 0.0f}, q);
  cam.fovy = o.fovy;
  cam.projection = CAMERA_PERSPECTIVE;
  return cam;
}
}  // namespace

int runSunShot(const NativeAssets& native, const std::string& clusterDir,
               const std::string& outPrefix, const SunShotOptions& options) {
  InitWindow(options.width, options.height, "X4 Station Builder - sunshot");
  SetTargetFPS(60);

  {  // GL-owning locals destruct before CloseWindow (snaptest.cpp pattern).
    Sky sky{native, clusterDir};
    if (!sky.isValid()) {
      std::fprintf(stderr, "sunshot: cluster '%s' did not load (procedural fallback)\n",
                   clusterDir.c_str());
    }
    const ::Vector3 sunDir = sky.sun().dir;
    TraceLog(LOG_INFO, "sunshot: %s sun dir = %.3f %.3f %.3f", clusterDir.c_str(),
             static_cast<double>(sunDir.x), static_cast<double>(sunDir.y),
             static_cast<double>(sunDir.z));

    FramePipeline pipeline;
    pipeline.setSceneExposure(options.sceneExposure);
    sky.setGlowEnabled(options.drawGlow);
    const ::Camera3D cam = aimCamera(sunDir, options);

    // Warm-up frames before the shot (uishot.cpp pattern): double buffering can
    // hand TakeScreenshot a stale back buffer after a single EndDrawing.
    for (int i = 0; i < 3; ++i) {
      BeginDrawing();
      pipeline.begin();
      ClearBackground(BLACK);
      BeginMode3D(cam);
      sky.draw(cam);
      EndMode3D();
      pipeline.end([&] {
        if (options.drawFlares) sky.drawLensFlares(cam, GetScreenWidth(), GetScreenHeight());
      });
      EndDrawing();
    }
    TakeScreenshot((outPrefix + "_sun.png").c_str());
  }

  CloseWindow();
  return 0;
}

}  // namespace x4sb::editor

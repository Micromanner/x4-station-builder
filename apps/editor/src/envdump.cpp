#include "envdump.hpp"

#include "env_cubemap.hpp"
#include "external/glad.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "sky.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace x4sb::editor {

namespace {
constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;

// Mirror-ball probe: world normal reflected about the view ray, sampled from
// the cubemap. If the bake orientation is right, the ball shows the sky that
// is BEHIND/AROUND it on matching sides.
constexpr const char* kProbeVs = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
out vec3 fragPos;
out vec3 fragNormal;
void main() {
  fragPos = vec3(matModel * vec4(vertexPosition, 1.0));
  fragNormal = mat3(matModel) * vertexNormal;
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";
constexpr const char* kProbeFs = R"(#version 330
in vec3 fragPos;
in vec3 fragNormal;
uniform samplerCube envMap;
uniform vec3 cameraPos;
out vec4 finalColor;
void main() {
  vec3 v = normalize(cameraPos - fragPos);
  vec3 r = reflect(-v, normalize(fragNormal));
  finalColor = vec4(textureLod(envMap, r, 0.0).rgb, 1.0);
}
)";

}  // namespace

int runEnvDump(const NativeAssets& native, const std::string& outPrefix) {
  InitWindow(kScreenW, kScreenH, "X4 Station Builder - envdump");
  SetTargetFPS(60);

  {  // Inner scope: every GL-owning local (Sky, EnvCubemap, shader, mesh)
     // destructs before CloseWindow below — mirrors snaptest.cpp.
    Sky sky{native};
    const ::Vector3 sunDir = sky.sun().dir;
    TraceLog(LOG_INFO, "envdump: sun dir = %.3f %.3f %.3f", static_cast<double>(sunDir.x),
             static_cast<double>(sunDir.y), static_cast<double>(sunDir.z));
    EnvCubemap env{sky};

    // Face dumps: GL converts fp16 -> u8 on readback.
    std::vector<unsigned char> buf(static_cast<std::size_t>(EnvCubemap::kFaceSize) *
                                   EnvCubemap::kFaceSize * 4);
    glBindTexture(GL_TEXTURE_CUBE_MAP, env.textureId());
    for (unsigned int f = 0; f < 6; ++f) {
      glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
      ::Image img{buf.data(), EnvCubemap::kFaceSize, EnvCubemap::kFaceSize, 1,
                  PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
      const std::string name = outPrefix + "_face" + std::to_string(f) + ".png";
      ExportImage(img, name.c_str());  // Image wraps buf; no UnloadImage
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    // Probe shot: sky behind + mirror ball, plain backbuffer (no HDR pipeline —
    // this is an orientation check, not a lighting check).
    ::Shader probe = LoadShaderFromMemory(kProbeVs, kProbeFs);
    ::Mesh sphere = GenMeshSphere(1.0f, 32, 32);
    ::Material mat = LoadMaterialDefault();
    mat.shader = probe;
    const int unit = 9;
    SetShaderValue(probe, GetShaderLocation(probe, "envMap"), &unit, SHADER_UNIFORM_INT);
    rlActiveTextureSlot(9);
    rlEnableTextureCubemap(env.textureId());
    rlActiveTextureSlot(0);

    ::Camera3D cam{};
    cam.position = ::Vector3{0.0f, 0.5f, -3.0f};
    cam.target = ::Vector3{0.0f, 0.0f, 0.0f};
    cam.up = ::Vector3{0.0f, 1.0f, 0.0f};
    cam.fovy = 60.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    const std::array<float, 3> cpos{cam.position.x, cam.position.y, cam.position.z};
    SetShaderValue(probe, GetShaderLocation(probe, "cameraPos"), cpos.data(), SHADER_UNIFORM_VEC3);

    // Warm-up frames before the shot (uishot.cpp pattern): with double
    // buffering, TakeScreenshot right after a single EndDrawing can read a
    // stale/blank back buffer, so draw a few frames and only shoot the last.
    for (int i = 0; i < 3; ++i) {
      BeginDrawing();
      ClearBackground(BLACK);
      BeginMode3D(cam);
      sky.draw(cam);
      DrawMesh(sphere, mat, MatrixIdentity());
      EndMode3D();
      EndDrawing();
    }
    TakeScreenshot((outPrefix + "_probe.png").c_str());

    UnloadMesh(sphere);
    // mat is LoadMaterialDefault()'s copy sharing probe's GL id (mat.shader =
    // probe above) plus the global default texture maps — UnloadMaterial would
    // free that shared default texture, so unload only the shader we loaded.
    UnloadShader(probe);
  }

  CloseWindow();  // after all GL-owning locals destruct
  return 0;
}

}  // namespace x4sb::editor

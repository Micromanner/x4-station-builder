#include "env_cubemap.hpp"

#include "bloom.hpp"
#include "external/glad.h"
#include "raymath.h"
#include "sky.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace x4sb::editor {

namespace {

// GL cubemap face camera basis (positive_x + i). The -Y ups encode GL's
// top-left cubemap convention. VERIFIED via --envdump (IBL task 1): with
// sun dir ~(0.26, 0.60, -0.76), the sun disc lands in the +X/+Y/-Z face
// corners nearest that vector (faces 0, 2, 5) exactly as the dot products
// predict, and the probe's mirror ball shows the sun near its center when
// the camera looks roughly opposite the sun direction — this table is
// frozen; do not "fix" it without re-running the harness.
struct FaceBasis {
  ::Vector3 target;
  ::Vector3 up;
};
constexpr std::array<FaceBasis, 6> kFaces{{
    {{1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}},   // +X
    {{-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}},  // -X
    {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},    // +Y
    {{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},  // -Y
    {{0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},   // +Z
    {{0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}},  // -Z
}};

void setCubeParams() {
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
}

}  // namespace

unsigned int makeFallbackCubemap() {
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_CUBE_MAP, id);
  const std::array<unsigned char, 4> black{0, 0, 0, 255};
  for (unsigned int f = 0; f < 6; ++f)
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, black.data());
  setCubeParams();
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // single mip
  glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
  return id;
}

EnvCubemap::EnvCubemap(Sky& sky) {
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_CUBE_MAP, id);
  // Defined black by construction, not just allocated: upload zero-filled RGBA8
  // data (GL converts into the RGBA16F internal format) rather than nullptr, so
  // the cube is mipmap-complete even if rebake() below bails out (RT failure).
  const std::vector<unsigned char> zeros(static_cast<std::size_t>(kFaceSize) * kFaceSize * 4, 0);
  for (unsigned int f = 0; f < 6; ++f)
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F, kFaceSize, kFaceSize, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, zeros.data());
  setCubeParams();
  glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
  glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
  id_ = id;
  // log2(kFaceSize): the 1x1 mip is the level X4 samples as irradiance.
  maxLod_ = std::log2(static_cast<float>(kFaceSize));
  rebake(sky);
}

EnvCubemap::~EnvCubemap() {
  if (id_ != 0) {
    GLuint id = id_;
    glDeleteTextures(1, &id);
  }
}

void EnvCubemap::rebake(Sky& sky) {
  // Depth for the planet pass: bodies occlude atmospheres and rings.
  ::RenderTexture2D rt = loadHdrTarget(kFaceSize, kFaceSize, /*withDepth=*/true);
  if (rt.id == 0 || id_ == 0) {
    TraceLog(LOG_WARNING, "EnvCubemap: bake target unavailable — env stays black");
    if (rt.id != 0) unloadHdrTarget(rt);
    return;
  }
  for (unsigned int f = 0; f < 6; ++f) {
    ::Camera3D cam{};
    cam.position = ::Vector3{0.0f, 0.0f, 0.0f};
    cam.target = kFaces[f].target;
    cam.up = kFaces[f].up;
    cam.fovy = 90.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    BeginTextureMode(rt);
    ClearBackground(::Color{0, 0, 0, 255});
    BeginMode3D(cam);
    sky.draw(cam);
    EndMode3D();
    EndTextureMode();
    // Copy the face out of the RT's fbo. Same GL row order on both sides, so
    // no flip here — orientation is owned by kFaces and verified by --envdump.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rt.id);
    glBindTexture(GL_TEXTURE_CUBE_MAP, id_);
    glCopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, 0, 0, kFaceSize, kFaceSize);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }
  glBindTexture(GL_TEXTURE_CUBE_MAP, id_);
  glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
  glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
  unloadHdrTarget(rt);
}

}  // namespace x4sb::editor

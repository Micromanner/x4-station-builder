#include "nebula_fog.hpp"

#include "dds_texture.hpp"
#include "external/glad.h"
#include "native_assets.hpp"
#include "profiling.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "x4sb/archive/archive.hpp"
#include "x4sb/assetpipe/native_background.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace x4sb::editor {
namespace {

// Port of X4's fog path draw (X4_Station_FRONT.rdc 41392: shaders in
// tools/rd_frame/X4_Station_FRONT/fog/fog_41392_{vs,fs}.glsl). View space, camera
// at the origin; sphere centres arrive relative to the station, in display space.
constexpr const char* kPathVs = R"(#version 330
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec4 sphere;  // centre, radius
layout(location = 2) in float density;
uniform mat4 viewRot;
uniform mat4 projection;
uniform vec3 camPos;
flat out vec3 vCenter;
flat out float vRadius;
flat out float vDensity;
void main() {
  vec3 c = sphere.xyz - camPos;
  vCenter = (viewRot * vec4(c, 1.0)).xyz;
  vRadius = sphere.w;
  vDensity = density;
  // X4's proxy is the unit icosphere scaled by 1.1 r, so its faces enclose the sphere.
  gl_Position = projection * viewRot * vec4(c + vertexPosition * sphere.w * 1.1, 1.0);
}
)";

constexpr const char* kPathFs = R"(#version 330
flat in vec3 vCenter;
flat in float vRadius;
flat in float vDensity;
out vec4 outPath;
uniform sampler2D depthTex;
uniform sampler2D pattern;
uniform vec2 proj;  // P00, P11
uniform vec2 clip;  // near, far
uniform vec2 screen;
uniform mat3 viewToX4;
uniform vec4 channel;
// X4's froxel volume owns the first 6 km (V_volume_range.y).
const float kNear = 6000.0;
float tex(vec2 p) { return texture(pattern, (p + 1.0) * 0.5).r; }
void main() {
  vec2 ndc = gl_FragCoord.xy / screen * 2.0 - 1.0;
  vec3 dir = normalize(vec3(ndc.x / proj.x, ndc.y / proj.y, -1.0));
  float d = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
  float sceneDist = 1e30;
  if (d < 1.0) {
    float z = d * 2.0 - 1.0;
    sceneDist = 2.0 * clip.x * clip.y / (clip.y + clip.x - z * (clip.y - clip.x)) / -dir.z;
  }
  float r = vRadius;
  float tc = dot(dir, vCenter);
  float disc = tc * tc - (dot(vCenter, vCenter) - r * r);
  if (disc <= 1e-4) discard;
  float h = sqrt(disc);
  float path = (min(sceneDist, max(kNear, tc + h)) - min(sceneDist, max(kNear, tc - h))) *
               smoothstep(min(r * 8.0, kNear - min(2.0 * r, kNear * 0.25)), min(r * 14.0, kNear),
                          tc + h) * (h / (2.0 * r));
  if (path <= 0.0) discard;
  vec3 p = viewToX4 * (dir * tc - vCenter) / (r * 1.1);
  vec3 w = clamp(1.0 - abs(p), 0.0, 1.0);
  float shape = (tex(p.yz) * w.x + tex(p.xy) * w.z + tex(p.xz) * w.y) / 3.0;
  outPath = channel * (path * shape * vDensity * 0.001);
}
)";

// Port of X4's composite (41658, composite_41658_fs.glsl) with the Fourier
// self-shadow at 1 (~1 on sky in the capture) and no froxel term. Transmittance
// is the product of its channels, as in X4, so the blend can apply it.
constexpr const char* kCompositeVs = R"(#version 330
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kCompositeFs = R"(#version 330
out vec4 outColor;
uniform sampler2D pathTex;
uniform vec2 proj;
uniform vec2 screen;
uniform int mediaCount;
uniform vec3 sigmaExt[3];
uniform vec4 phaseParam[9];  // per medium x phase: sigmaS, shape
uniform ivec4 phases[3];     // functions, count
uniform vec3 sunDirView;
uniform vec3 sunColor;
float phase(int fn, float g, float c) {
  if (fn == 0) return 0.0795774715;
  if (fn == 1) return 0.0596831036 * (1.0 + c * c);
  if (fn == 2) return 0.0795774715 * (1.0 - g * g) / pow(abs(1.0 + g * g - 2.0 * g * c), 1.5);
  float m = 0.5 * (1.0 + c);
  m *= m; m *= m; m *= m;
  if (fn == 3) return 0.0795774715 * (0.5 + 4.5 * m);
  m *= m; m *= m;
  return 0.0795774715 * (0.5 + 16.5 * m);
}
void main() {
  vec4 path = texelFetch(pathTex, ivec2(gl_FragCoord.xy), 0) * 1000.0;
  vec2 ndc = gl_FragCoord.xy / screen * 2.0 - 1.0;
  vec3 dir = normalize(vec3(ndc.x / proj.x, ndc.y / proj.y, -1.0));
  float c = dot(dir, sunDirView);
  vec3 tau = vec3(1e-9);
  vec3 scatter = vec3(0.0);
  for (int i = 0; i < mediaCount; ++i) {
    float L = path[i];
    tau += sigmaExt[i] * L;
    // X4 weights by scatter_power / sum(sigmaS) as well, which is 1.
    for (int k = 0; k < phases[i].w; ++k) {
      vec4 pp = phaseParam[i * 3 + k];
      scatter += pp.xyz * phase(phases[i][k], pp.w, c) * L;
    }
  }
  vec3 t = exp(-tau);
  outColor = vec4(sunColor * scatter * (1.0 - t) / tau, t.x * t.y * t.z);
}
)";

// Icosahedron subdivided once: X4's 42-vertex, 240-index fog proxy. Faces wind
// counter-clockwise seen from outside.
void icosphere(std::vector<float>& vertices, std::vector<unsigned int>& indices) {
  const float t = (1.0F + std::sqrt(5.0F)) / 2.0F;
  std::vector<::Vector3> v{{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
                           {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
                           {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
  for (auto& p : v) p = Vector3Normalize(p);
  const std::vector<std::array<unsigned int, 3>> faces{
      {0, 11, 5},  {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
      {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
      {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
  std::vector<std::pair<std::pair<unsigned int, unsigned int>, unsigned int>> mids;
  const auto midpoint = [&](unsigned int a, unsigned int b) {
    const auto key = std::make_pair(std::min(a, b), std::max(a, b));
    for (const auto& [k, index] : mids)
      if (k == key) return index;
    v.push_back(Vector3Normalize(Vector3Lerp(v[a], v[b], 0.5F)));
    const auto index = static_cast<unsigned int>(v.size() - 1);
    mids.emplace_back(key, index);
    return index;
  };
  for (const auto& f : faces) {
    const unsigned int a = midpoint(f[0], f[1]);
    const unsigned int b = midpoint(f[1], f[2]);
    const unsigned int c = midpoint(f[2], f[0]);
    for (const auto& tri : std::array<std::array<unsigned int, 3>, 4>{
             {{f[0], a, c}, {f[1], b, a}, {f[2], c, b}, {a, b, c}}})
      indices.insert(indices.end(), tri.begin(), tri.end());
  }
  for (const auto& p : v) vertices.insert(vertices.end(), {p.x, p.y, p.z});
}

int loc(const ::Shader& s, const char* name) { return GetShaderLocation(s, name); }

}  // namespace

NebulaFog::NebulaFog() {
  pathShader_ = LoadShaderFromMemory(kPathVs, kPathFs);
  compositeShader_ = LoadShaderFromMemory(kCompositeVs, kCompositeFs);

  std::vector<float> vertices;
  std::vector<unsigned int> indices;
  icosphere(vertices, indices);
  sphereIndexCount_ = static_cast<int>(indices.size());
  glGenVertexArrays(1, &sphereVao_);
  glGenBuffers(1, &sphereVbo_);
  glGenBuffers(1, &sphereEbo_);
  glGenBuffers(1, &instanceVbo_);
  glBindVertexArray(sphereVao_);
  glBindBuffer(GL_ARRAY_BUFFER, sphereVbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
               vertices.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphereEbo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)), indices.data(),
               GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glVertexAttribDivisor(1, 1);
  glVertexAttribDivisor(2, 1);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glGenVertexArrays(1, &fullscreenVao_);
}

NebulaFog::~NebulaFog() {
  unload();
  resize({0, 0});
  glDeleteVertexArrays(1, &sphereVao_);
  glDeleteVertexArrays(1, &fullscreenVao_);
  glDeleteBuffers(1, &sphereVbo_);
  glDeleteBuffers(1, &sphereEbo_);
  glDeleteBuffers(1, &instanceVbo_);
  UnloadShader(pathShader_);
  UnloadShader(compositeShader_);
}

void NebulaFog::unload() {
  for (const ::Texture2D& t : patterns_)
    if (t.id != 0) UnloadTexture(t);
  patterns_.clear();
  spheres_.clear();
  media_.clear();
  drawCounts_.clear();
  missingAssets_ = 0;
}

void NebulaFog::load(const assetpipe::FogField& field, const NativeAssets& native) {
  unload();
  // The path target has one channel and the composite one uniform slot per medium.
  media_.assign(field.media.begin(),
                field.media.begin() + static_cast<std::ptrdiff_t>(
                                          std::min(field.media.size(), assetpipe::kMaxFogMedia)));
  for (const assetpipe::FogSphere& s : field.spheres)
    if (s.medium >= 0 && static_cast<std::size_t>(s.medium) < media_.size()) spheres_.push_back(s);
  origin_ = field.origin;
  for (const assetpipe::FogMedium& m : media_) {
    ::Texture2D t{};
    if (const auto bytes = readTextureDds(archiveAssetReader(*native.archive), m.pattern))
      t = loadDdsFromMemory(*bytes, /*srgb=*/true);  // X4 samples it through an sRGB view
    if (t.id == 0) {
      ++missingAssets_;
      TraceLog(LOG_WARNING, "Fog: failed to load pattern %s", m.pattern.c_str());
    } else {
      SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
      SetTextureFilter(t, t.mipmaps > 1 ? TEXTURE_FILTER_TRILINEAR : TEXTURE_FILTER_BILINEAR);
    }
    patterns_.push_back(t);
  }
  uploadInstances();
}

void NebulaFog::setOrigin(Vec3 clusterPos) {
  origin_ = clusterPos;
  uploadInstances();
}

void NebulaFog::setSun(::Vector3 dir, ::Vector3 color) {
  sunDir_ = Vector3Normalize(dir);
  sunColor_ = color;
}

// Station-relative display space (X4 z negated), grouped by medium; spheres past
// the nebulafar range of the station are dropped as X4 drops them.
void NebulaFog::uploadInstances() {
  std::vector<float> data;
  drawCounts_.assign(media_.size(), 0);
  for (std::size_t m = 0; m < media_.size(); ++m) {
    for (const assetpipe::FogSphere& s : spheres_) {
      if (static_cast<std::size_t>(s.medium) != m) continue;
      const Vec3 rel = s.pos - origin_;
      if (length(rel) - s.radius > assetpipe::kFogRange) continue;
      data.insert(data.end(),
                  {static_cast<float>(rel.x), static_cast<float>(rel.y), static_cast<float>(-rel.z),
                   static_cast<float>(s.radius), s.density});
      ++drawCounts_[m];
    }
  }
  glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
               data.empty() ? nullptr : data.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void NebulaFog::resize(std::array<int, 2> dims) {
  if (pathFbo_ != 0) glDeleteFramebuffers(1, &pathFbo_);
  if (pathTex_ != 0) glDeleteTextures(1, &pathTex_);
  pathFbo_ = 0;
  pathTex_ = 0;
  dims_ = dims;
  if (dims[0] <= 0 || dims[1] <= 0) return;
  // One channel per medium; X4 stores path x 0.001 in R16F targets, same precision.
  glGenTextures(1, &pathTex_);
  glBindTexture(GL_TEXTURE_2D, pathTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, dims[0], dims[1], 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &pathFbo_);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pathFbo_);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pathTex_, 0);
}

void NebulaFog::apply(const ::Camera3D& camera, const SceneDepthCopy& depth) {
  if (!enabled_ || spheres_.empty() || media_.empty()) return;
  if (pathShader_.id == rlGetShaderIdDefault() || compositeShader_.id == rlGetShaderIdDefault())
    return;
  ::Matrix p = rlGetMatrixProjection();
  if (p.m15 != 0.0F) return;  // orthographic
  ZoneScopedN("fog");
  rlDrawRenderBatchActive();

  GLint drawFbo = 0;
  GLint program = 0;
  GLint vao = 0;
  GLint activeTex = 0;
  GLint cullMode = 0;
  GLint frontFace = 0;
  std::array<GLint, 4> viewport{};
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
  glGetIntegerv(GL_CULL_FACE_MODE, &cullMode);
  glGetIntegerv(GL_FRONT_FACE, &frontFace);
  glGetIntegerv(GL_VIEWPORT, viewport.data());
  const GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean cull = glIsEnabled(GL_CULL_FACE);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  GLboolean depthMask = GL_TRUE;
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

  const std::array<int, 2> dims{static_cast<int>(depth.size()[0]),
                                static_cast<int>(depth.size()[1])};
  if (dims != dims_ || pathTex_ == 0) resize(dims);
  if (pathFbo_ == 0) return;
  const float w = static_cast<float>(dims[0]);
  const float h = static_cast<float>(dims[1]);
  const auto nearC = static_cast<float>(rlGetCullDistanceNear());
  const auto farC = static_cast<float>(rlGetCullDistanceFar());

  // The view without its translation (sphere centres arrive camera-relative); its
  // rows are the camera's right, up and back axes.
  ::Matrix view = GetCameraMatrix(camera);
  view.m12 = view.m13 = view.m14 = 0.0F;
  const ::Vector3 right{view.m0, view.m4, view.m8};
  const ::Vector3 up{view.m1, view.m5, view.m9};
  const ::Vector3 back{view.m2, view.m6, view.m10};
  // View vector -> X4 world axes, for the pattern lookup X4 does in object space.
  const std::array<float, 9> viewToX4{right.x, right.y, -right.z, up.x,   up.y,
                                      -up.z,   back.x,  back.y,   -back.z};
  // Spheres reach 600 km: an infinite far plane keeps them unclipped.
  p.m10 = -1.0F;
  p.m14 = -2.0F * nearC;

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glViewport(0, 0, dims[0], dims[1]);

  {
    ZoneScopedN("fog: path");
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pathFbo_);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    // Back faces only: one fragment per sphere and pixel, also from inside it.
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_FRONT);
    rlSetBlendFactors(RL_ONE, RL_ONE, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM);

    const ::Shader& s = pathShader_;
    glUseProgram(s.id);
    glUniformMatrix4fv(loc(s, "viewRot"), 1, GL_FALSE, MatrixToFloatV(view).v);
    glUniformMatrix4fv(loc(s, "projection"), 1, GL_FALSE, MatrixToFloatV(p).v);
    glUniform3f(loc(s, "camPos"), camera.position.x, camera.position.y, camera.position.z);
    glUniform2f(loc(s, "proj"), p.m0, p.m5);
    glUniform2f(loc(s, "clip"), nearC, farC);
    glUniform2f(loc(s, "screen"), w, h);
    glUniformMatrix3fv(loc(s, "viewToX4"), 1, GL_FALSE, viewToX4.data());
    glUniform1i(loc(s, "depthTex"), 0);
    glUniform1i(loc(s, "pattern"), 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth.texture());
    glBindVertexArray(sphereVao_);
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
    constexpr GLsizei kStride = 5 * sizeof(float);
    std::size_t first = 0;
    for (std::size_t m = 0; m < media_.size(); ++m) {
      const int count = drawCounts_[m];
      if (count == 0) continue;
      // GL 3.3 has no base instance: point the instance attributes at this medium.
      const auto offset = static_cast<std::uintptr_t>(first * kStride);
      glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, kStride,
                            reinterpret_cast<const void*>(offset));
      glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, kStride,
                            reinterpret_cast<const void*>(offset + 4 * sizeof(float)));
      std::array<float, 4> channel{};
      channel[m] = 1.0F;
      glUniform4fv(loc(s, "channel"), 1, channel.data());
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D,
                    patterns_[m].id != 0 ? patterns_[m].id : rlGetTextureIdDefault());
      glDrawElementsInstanced(GL_TRIANGLES, sphereIndexCount_, GL_UNSIGNED_INT, nullptr, count);
      first += static_cast<std::size_t>(count);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }

  {
    ZoneScopedN("fog: composite");
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glDisable(GL_CULL_FACE);
    // dst = inscatter + dst x transmittance; alpha kept.
    rlSetBlendFactorsSeparate(RL_ONE, RL_SRC_ALPHA, RL_ZERO, RL_ONE, RL_FUNC_ADD, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);

    const ::Shader& s = compositeShader_;
    glUseProgram(s.id);
    std::array<float, 9> sigmaExt{};
    std::array<float, 36> phaseParam{};
    std::array<int, 12> phases{};
    for (std::size_t m = 0; m < media_.size(); ++m) {
      const assetpipe::FogMedium& medium = media_[m];
      for (std::size_t c = 0; c < 3; ++c) sigmaExt[m * 3 + c] = medium.sigmaExt[c];
      for (std::size_t k = 0; k < medium.phases.size(); ++k) {
        const assetpipe::FogPhase& ph = medium.phases[k];
        const std::size_t i = (m * 3 + k) * 4;
        phaseParam[i] = ph.sigmaS[0];
        phaseParam[i + 1] = ph.sigmaS[1];
        phaseParam[i + 2] = ph.sigmaS[2];
        phaseParam[i + 3] = ph.shape;
        phases[m * 4 + k] = static_cast<int>(ph.function);
      }
      phases[m * 4 + 3] = static_cast<int>(medium.phases.size());
    }
    const ::Vector3 sunView{Vector3DotProduct(right, sunDir_), Vector3DotProduct(up, sunDir_),
                            Vector3DotProduct(back, sunDir_)};
    glUniform1i(loc(s, "pathTex"), 0);
    glUniform2f(loc(s, "proj"), p.m0, p.m5);
    glUniform2f(loc(s, "screen"), w, h);
    glUniform1i(loc(s, "mediaCount"), static_cast<int>(media_.size()));
    glUniform3fv(loc(s, "sigmaExt"), 3, sigmaExt.data());
    glUniform4fv(loc(s, "phaseParam"), 9, phaseParam.data());
    glUniform4iv(loc(s, "phases"), 3, phases.data());
    glUniform3f(loc(s, "sunDirView"), sunView.x, sunView.y, sunView.z);
    glUniform3f(loc(s, "sunColor"), sunColor_.x, sunColor_.y, sunColor_.z);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pathTex_);
    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

  rlSetBlendMode(RL_BLEND_ALPHA);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(static_cast<GLenum>(activeTex));
  glBindVertexArray(static_cast<GLuint>(vao));
  glUseProgram(static_cast<GLuint>(program));
  glCullFace(static_cast<GLenum>(cullMode));
  glFrontFace(static_cast<GLenum>(frontFace));
  if (cull == GL_TRUE) glEnable(GL_CULL_FACE);
  if (depthTest == GL_TRUE) glEnable(GL_DEPTH_TEST);
  if (blend == GL_FALSE) glDisable(GL_BLEND);
  glDepthMask(depthMask);
}

}  // namespace x4sb::editor

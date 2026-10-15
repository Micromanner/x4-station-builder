#include "ssao.hpp"

#include "external/glad.h"
#include "rlgl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace x4sb::editor {
namespace {

constexpr const char* kFullscreenVs = R"(#version 330
out vec2 uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Shared by both passes: view-space position from the resolved depth, in km
// like X4 (DEPTH_SCALE 0.001) so its constants keep their meaning. z is the
// distance along the view axis, x right, y up.
constexpr const char* kHeader = R"(#version 330
in vec2 uv;
out vec4 outAo;
uniform sampler2D depthTex;
uniform vec2 proj;  // P00, P11
uniform vec2 clip;  // near, far
float viewZ(vec2 t) {
  float z = textureLod(depthTex, t, 0.0).r * 2.0 - 1.0;
  return 2.0 * clip.x * clip.y / (clip.y + clip.x - z * (clip.y - clip.x)) * 0.001;
}
vec3 viewPos(vec2 t) {
  float z = viewZ(t);
  vec2 ndc = t * 2.0 - 1.0;
  return vec3(ndc.x * z / proj.x, ndc.y * z / proj.y, z);
}
)";

constexpr const char* kAoFs = R"(
uniform vec2 aoSize;
uniform vec2 depthTexel;
const float kSampleRadius = 40.0;
const float kIntensity = 2.2;
const float kScale = 2.25;
const float kBias = 0.1;
const float kClipFar = 15000.0 * 0.001;

// X4 reads a G-buffer normal; here the face normal comes from the depth, each
// axis from the neighbour on the same surface (the smaller depth step).
vec3 faceNormal(vec3 p) {
  vec3 r = viewPos(uv + vec2(depthTexel.x, 0.0)) - p;
  vec3 l = p - viewPos(uv - vec2(depthTexel.x, 0.0));
  vec3 u = viewPos(uv + vec2(0.0, depthTexel.y)) - p;
  vec3 d = p - viewPos(uv - vec2(0.0, depthTexel.y));
  vec3 n = normalize(cross(abs(r.z) < abs(l.z) ? r : l, abs(u.z) < abs(d.z) ? u : d));
  return dot(n, p) > 0.0 ? -n : n;
}

void main() {
  if (textureLod(depthTex, uv, 0.0).r >= 1.0) { outAo = vec4(1.0); return; }
  vec3 origin = viewPos(uv);
  if (origin.z > kClipFar) { outAo = vec4(1.0); return; }
  vec3 normal = faceNormal(origin);

  ivec2 fc = ivec2(gl_FragCoord.xy);
  vec3 rvec = kNoise[(fc.x % 4) + (fc.y % 4) * 4];
  vec3 tangent = normalize(rvec - normal * dot(rvec, normal));
  mat3 tbn = mat3(tangent, cross(normal, tangent), normal);

  // Screen-constant in effect: ~5% of the view distance past 5 m.
  float radius = kSampleRadius / aoSize.x * 0.0125 * max(origin.z / (5.0 * 0.001), 1.0);
  float occlusion = 0.0;
  for (int i = 0; i < 32; ++i) {
    vec3 s = tbn * kKernel[i] * radius + origin;
    vec2 t = vec2(s.x * proj.x, s.y * proj.y) / s.z * 0.5 + 0.5;
    if (t.x < 0.0 || t.x > 1.0 || t.y < 0.0 || t.y > 1.0) continue;
    if (length(t - uv) <= length(1.0 / aoSize)) continue;  // same pixel
    vec3 p2 = viewPos(t);
    float dz = abs(origin.z - p2.z);
    // An occluder far in front of the sample (another module) does not count.
    float range = dz >= 1.5 * radius ? 0.0 : 1.0;
    range *= clamp(dot(normal, normalize(p2 - origin)) - kBias, 0.0, 1.0);
    range *= 1.0 / (1.0 + dz * kScale);
    occlusion += (p2.z <= s.z ? 1.0 : 0.0) * range;
  }
  // X4 squares the unclamped value; the clamp keeps GLSL pow() defined.
  float ao = clamp(1.0 - occlusion / 32.0 * kIntensity, 0.0, 1.0);
  outAo = vec4(ao * ao);
}
)";

// SplitMix64 (Steele et al. 2014): the tables must be identical on every build,
// which <random> distributions do not guarantee.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
  // Uniform in [0, 1) from the top 53 bits.
  [[nodiscard]] double next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return static_cast<double>(z >> 11U) * 0x1.0p-53;
  }

 private:
  std::uint64_t state_;
};

void appendVec3(std::string& out, double x, double y, double z) {
  std::array<char, 96> buf{};
  std::snprintf(buf.data(), buf.size(), "vec3(%.6g, %.6g, %.6g)", x, y, z);
  out += buf.data();
}

// kNoise: random in-plane rotations tiled 4x4 over the screen. kKernel: directions
// uniform on the +z hemisphere, lengths growing quadratically so most samples
// stay near the origin (Chapman 2013), with a floor against self-occlusion.
std::string aoTables() {
  constexpr double kTwoPi = 6.283185307179586;
  constexpr int kNoiseSize = 16;
  constexpr int kKernelSize = 32;
  SplitMix64 rng(0x55A0'5EEDULL);
  std::string out = "const vec3 kNoise[16] = vec3[](";
  for (int i = 0; i < kNoiseSize; ++i) {
    const double a = kTwoPi * rng.next();
    appendVec3(out, std::cos(a), std::sin(a), 0.0);
    out += i + 1 < kNoiseSize ? ", " : ");\n";
  }
  out += "const vec3 kKernel[32] = vec3[](";
  for (int i = 0; i < kKernelSize; ++i) {
    const double z = rng.next();
    const double phi = kTwoPi * rng.next();
    const double r = std::sqrt(1.0 - z * z);
    const double f = static_cast<double>(i + 1) / kKernelSize;
    const double len = std::max(f * f, 1.0 / 160.0);
    appendVec3(out, len * r * std::cos(phi), len * r * std::sin(phi), len * z);
    out += i + 1 < kKernelSize ? ", " : ");\n";
  }
  return out;
}

// X4's DepthAwareBlurOld with the interpolated 8-tap kernel; its stop at
// "complex" (MSAA edge) pixels becomes a stop at a depth discontinuity.
constexpr const char* kBlurFs = R"(
uniform sampler2D aoTex;
uniform vec2 step;
const float kOffsets[9] = float[](-7.30294037, -5.35180569, -3.40398502, -1.45842957, 0.0,
                                  1.45842957, 3.40398502, 5.35180569, 7.30294037);
const float kWeights[9] = float[](0.000229, 0.005977, 0.060598, 0.241732, 0.382928,
                                  0.241732, 0.060598, 0.005977, 0.000229);

void main() {
  float z = viewZ(uv);
  float weight = kWeights[4];
  float ao = weight * texture(aoTex, uv).r;
  for (int i = 3; i >= 0; --i) {
    vec2 t = uv + step * kOffsets[i];
    if (abs(viewZ(t) - z) > 0.05 * z) break;
    ao += kWeights[i] * texture(aoTex, t).r;
    weight += kWeights[i];
  }
  for (int i = 5; i <= 8; ++i) {
    vec2 t = uv + step * kOffsets[i];
    if (abs(viewZ(t) - z) > 0.05 * z) break;
    ao += kWeights[i] * texture(aoTex, t).r;
    weight += kWeights[i];
  }
  outAo = vec4(ao / weight);
}
)";

// X4 runs the pass at 1920x1080 for a 2560x1440 frame (station.rdc).
constexpr float kResolutionScale = 0.75F;

void setInt(const ::Shader& s, const char* name, int v) {
  glUniform1i(GetShaderLocation(s, name), v);
}
void setVec2(const ::Shader& s, const char* name, float x, float y) {
  glUniform2f(GetShaderLocation(s, name), x, y);
}

}  // namespace

Ssao::Ssao() {
  aoShader_ = LoadShaderFromMemory(kFullscreenVs, (kHeader + aoTables() + kAoFs).c_str());
  blurShader_ = LoadShaderFromMemory(kFullscreenVs, (std::string(kHeader) + kBlurFs).c_str());
  glGenVertexArrays(1, &vao_);
}

Ssao::~Ssao() {
  release();
  if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
  UnloadShader(aoShader_);
  UnloadShader(blurShader_);
}

void Ssao::release() {
  glDeleteFramebuffers(2, fbo_.data());
  glDeleteTextures(2, tex_.data());
  fbo_ = {};
  tex_ = {};
  dims_ = {};
}

void Ssao::resize(std::array<int, 2> dims) {
  release();
  glGenTextures(2, tex_.data());
  glGenFramebuffers(2, fbo_.data());
  for (std::size_t i = 0; i < 2; ++i) {
    glBindTexture(GL_TEXTURE_2D, tex_[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, dims[0], dims[1], 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_[i]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_[i], 0);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  dims_ = dims;
}

bool Ssao::compute() {
  if (aoShader_.id == rlGetShaderIdDefault() || blurShader_.id == rlGetShaderIdDefault())
    return false;
  const ::Matrix p = rlGetMatrixProjection();
  if (p.m15 != 0.0F) return false;  // orthographic
  rlDrawRenderBatchActive();
  if (!depth_.capture()) return false;

  GLint drawFbo = 0;
  GLint readFbo = 0;
  GLint program = 0;
  GLint vao = 0;
  GLint activeTex = 0;
  std::array<GLint, 4> viewport{};
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
  glGetIntegerv(GL_VIEWPORT, viewport.data());
  const GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  const GLboolean cull = glIsEnabled(GL_CULL_FACE);

  const std::array<float, 2>& full = depth_.size();
  const std::array<int, 2> dims{
      std::max(1, static_cast<int>(std::lround(full[0] * kResolutionScale))),
      std::max(1, static_cast<int>(std::lround(full[1] * kResolutionScale)))};
  if (dims != dims_ || tex_[0] == 0) resize(dims);
  const float w = static_cast<float>(dims[0]);
  const float h = static_cast<float>(dims[1]);
  const auto nearC = static_cast<float>(rlGetCullDistanceNear());
  const auto farC = static_cast<float>(rlGetCullDistanceFar());

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glViewport(0, 0, dims[0], dims[1]);
  glBindVertexArray(vao_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, depth_.texture());
  glActiveTexture(GL_TEXTURE1);

  const auto common = [&](const ::Shader& s) {
    glUseProgram(s.id);
    setInt(s, "depthTex", 0);
    setVec2(s, "proj", p.m0, p.m5);
    setVec2(s, "clip", nearC, farC);
  };

  // AO into [1], blur X into [0], blur Y into [1]; the swap makes [0] the result.
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_[1]);
  common(aoShader_);
  setVec2(aoShader_, "aoSize", w, h);
  setVec2(aoShader_, "depthTexel", 1.0F / full[0], 1.0F / full[1]);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  common(blurShader_);
  setInt(blurShader_, "aoTex", 1);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_[0]);
  glBindTexture(GL_TEXTURE_2D, tex_[1]);
  setVec2(blurShader_, "step", 1.0F / w, 0.0F);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_[1]);
  glBindTexture(GL_TEXTURE_2D, tex_[0]);
  setVec2(blurShader_, "step", 0.0F, 1.0F / h);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  std::swap(tex_[0], tex_[1]);
  std::swap(fbo_[0], fbo_[1]);

  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(static_cast<GLenum>(activeTex));
  glBindVertexArray(static_cast<GLuint>(vao));
  glUseProgram(static_cast<GLuint>(program));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  if (depthTest == GL_TRUE) glEnable(GL_DEPTH_TEST);
  if (blend == GL_TRUE) glEnable(GL_BLEND);
  if (cull == GL_TRUE) glEnable(GL_CULL_FACE);
  return true;
}

}  // namespace x4sb::editor

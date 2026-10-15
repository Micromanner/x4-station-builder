#include "sun_shadow.hpp"

#include "content_key.hpp"
#include "external/glad.h"
#include "raymath.h"
#include "render.hpp"
#include "rlgl.h"
#include "x4sb/editorcore/display_flip.hpp"

#include <cmath>

namespace x4sb::editor {
namespace {

// X4's cascades, identical in every 2026-10-10 capture (M_shadowCSM0-4 and the
// shadow passes' raster state): a light-aligned cube of `extent` metres for both
// the map and its depth range, centred `ahead` metres along the view axis, on a
// `size`^2 D16 map drawn with no culling, depth clamp and hardware depth bias.
// The bias constant is in units of the format's depth step, so it scales by 2^8
// onto our 24-bit maps; the slope term is what keeps the PCF kernels from
// self-shadowing sloped hulls.
struct CascadeSpec {
  float extent;
  float ahead;
  int size;
  float biasSlope;
  float biasConstant;
};
constexpr std::array<CascadeSpec, SunShadow::kCascades> kSpecs{{
    {8.7f, 2.1f, 2048, 20.5f, 3.5f},
    {33.1f, 10.1f, 2048, 2.85f, 71.7f},
    {377.4f, 97.8f, 3072, 3.18f, 7.2f},
    {2945.7f, 796.0f, 4096, 3.3f, 14.9f},
    // X4 centres this one 5.8 km ahead too; centred on the station instead it
    // covers the same hulls at viewing distance without re-rendering per frame.
    {21240.5f, 0.0f, 4096, 2.75f, 5.45f},
}};
constexpr int kFar = SunShadow::kCascades - 1;
constexpr float kD16ToD24 = 256.0f;

// X4's light-space basis (captures): u = cross(worldUp, away-from-sun). raylib's
// lookAt with up = +Y builds the same axes, so the maps match X4's orientation.
struct LightBasis {
  ::Vector3 toSun;
  ::Vector3 up;  // the lookAt up hint
  ::Vector3 right;
  ::Vector3 mapUp;
};
[[nodiscard]] LightBasis lightBasis(::Vector3 sunDir) {
  LightBasis b{};
  b.toSun = Vector3Normalize(sunDir);
  b.up = std::fabs(b.toSun.y) < 0.99f ? ::Vector3{0.0f, 1.0f, 0.0f} : ::Vector3{1.0f, 0.0f, 0.0f};
  b.right = Vector3Normalize(Vector3CrossProduct(b.up, b.toSun));
  b.mapUp = Vector3CrossProduct(b.toSun, b.right);
  return b;
}

// Whole-texel steps in light space, so a near cascade following the camera doesn't
// shimmer and re-renders only when it actually moves a texel.
[[nodiscard]] ::Vector3 snapToTexels(::Vector3 p, const LightBasis& b, float texel) {
  const auto snap = [texel](float v) { return std::round(v / texel) * texel; };
  return Vector3Add(Vector3Add(Vector3Scale(b.right, snap(Vector3DotProduct(p, b.right))),
                               Vector3Scale(b.mapUp, snap(Vector3DotProduct(p, b.mapUp)))),
                    Vector3Scale(b.toSun, snap(Vector3DotProduct(p, b.toSun))));
}

// The pass is ~9 ms CPU and ~15 ms GPU for the far cascade at 4096^2 on a
// 584-module plan (known-issues 1.5) while the station and sun are static between
// edits, so a cascade is skipped while its key is unchanged.
[[nodiscard]] std::uint64_t sceneKey(const Station& station, const MeshCache& meshes,
                                     ::Vector3 sunDir) {
  ContentKey k;
  k.value(sunDir);
  k.value(meshes.resolvedCount());
  k.value(station.revision());
  return k.result();
}

[[nodiscard]] Vec3 toVec3(::Vector3 v) {
  return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

[[nodiscard]] ::RenderTexture2D makeDepthTarget(int size) {
  // raylib's shadowmap recipe: an FBO with only a depth TEXTURE attached (GL 3.x
  // allows colour-less framebuffers), so the depth can be sampled as a sampler2D.
  ::RenderTexture2D t{};
  t.id = rlLoadFramebuffer();
  if (t.id == 0) return t;
  t.texture.width = size;  // BeginTextureMode sizes the viewport from these
  t.texture.height = size;
  rlEnableFramebuffer(t.id);
  t.depth.id = rlLoadTextureDepth(size, size, false);
  t.depth.width = size;
  t.depth.height = size;
  t.depth.format = 19;  // raylib's DEPTH_COMPONENT_24BIT bookkeeping value
  t.depth.mipmaps = 1;
  rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
  const bool ok = rlFramebufferComplete(t.id);
  rlDisableFramebuffer();
  // Hardware depth compare + bilinear: the shader's PCF taps are sampler2DShadow
  // lookups like X4's, each already a 2x2 filtered comparison.
  glBindTexture(GL_TEXTURE_2D, t.depth.id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (!ok) {
    TraceLog(LOG_WARNING, "SunShadow: %dpx depth FBO incomplete, cascade disabled", size);
    rlUnloadFramebuffer(t.id);
    rlUnloadTexture(t.depth.id);
    return ::RenderTexture2D{};
  }
  return t;
}

// One cascade's depth pass: an orthographic cube of `spec.extent` around `center`.
[[nodiscard]] ::Matrix renderCascade(const Station& station, const ModuleCatalog& catalog,
                                     MeshCache& meshes, const ::RenderTexture2D& target,
                                     const CascadeSpec& spec, const LightBasis& basis,
                                     ::Vector3 center, bool cullToRegion,
                                     const ::Camera3D* lodCamera) {
  ::Camera3D light{};
  light.position = Vector3Add(center, Vector3Scale(basis.toSun, spec.extent));
  light.target = center;
  light.up = basis.up;
  light.fovy = spec.extent;  // orthographic: the full vertical extent
  light.projection = CAMERA_ORTHOGRAPHIC;
  const ShadowRegion region{toVec3(center), toVec3(basis.right), toVec3(basis.mapUp),
                            toVec3(basis.toSun), 0.5 * static_cast<double>(spec.extent)};
  const double texel = static_cast<double>(spec.extent) / spec.size;

  // rlgl's clip planes are global state the sky/scene draws after this pass
  // depend on: set ours for the light projection, restore theirs after.
  const double savedNear = rlGetCullDistanceNear();
  const double savedFar = rlGetCullDistanceFar();
  BeginTextureMode(target);
  ClearBackground(WHITE);  // clears depth to 1.0
  rlSetClipPlanes(0.5 * static_cast<double>(spec.extent), 1.5 * static_cast<double>(spec.extent));
  BeginMode3D(light);
  const ::Matrix lightVP = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
  meshes.setShadowPass(true);
  rlDisableBackfaceCulling();
  glEnable(GL_DEPTH_CLAMP);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(spec.biasSlope, spec.biasConstant * kD16ToD24);
  // A part smaller than a shadow texel can't cast a resolvable shadow.
  drawShadowCasters(station, catalog, meshes, texel, cullToRegion ? &region : nullptr, lodCamera);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glDisable(GL_DEPTH_CLAMP);
  meshes.setShadowPass(false);
  rlEnableBackfaceCulling();
  EndMode3D();
  EndTextureMode();
  rlSetClipPlanes(savedNear, savedFar);
  return lightVP;
}

}  // namespace

SunShadow::SunShadow() {
  for (std::size_t i = 0; i < cascades_.size(); ++i)
    cascades_[i].target = makeDepthTarget(kSpecs[i].size);
}

SunShadow::~SunShadow() {
  for (const Cascade& c : cascades_) {
    if (c.target.id == 0) continue;
    rlUnloadTexture(c.target.depth.id);
    rlUnloadFramebuffer(c.target.id);
  }
}

void SunShadow::render(const Station& station, const ModuleCatalog& catalog, MeshCache& meshes,
                       ::Vector3 sunDir, const ::Camera3D& camera) {
  if (station.modules().empty()) {
    meshes.setShadowCascades(0);
    for (Cascade& c : cascades_) c.renderedKey = 0;
    return;
  }
  const LightBasis basis = lightBasis(sunDir);
  const std::uint64_t scene = sceneKey(station, meshes, sunDir);
  const ::Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
  int mask = 0;
  for (int i = 0; i < kCascades; ++i) {
    Cascade& c = cascades_[static_cast<std::size_t>(i)];
    if (c.target.id == 0) continue;
    const CascadeSpec& spec = kSpecs[static_cast<std::size_t>(i)];
    const bool near = i != kFar;
    const ::Vector3 nearCenter =
        near ? snapToTexels(Vector3Add(camera.position, Vector3Scale(forward, spec.ahead)), basis,
                            spec.extent / static_cast<float>(spec.size))
             : ::Vector3{};
    ContentKey k;
    k.value(scene);
    k.value(nearCenter);
    // Near cascades cull casters to the view frustum, so its shape is part of the key.
    if (near) {
      k.value(forward);
      k.value(camera.up);
      k.value(camera.fovy);
      k.value(GetScreenWidth());
      k.value(GetScreenHeight());
    }
    const std::uint64_t key = k.result();
    if (key != c.renderedKey) {
      c.renderedKey = key;
      ::Vector3 center = nearCenter;
      if (!near) {
        const Vec3 dc = flipZ(stationBounds(station, catalog).center);  // X4 -> display
        center = {static_cast<float>(dc.x), static_cast<float>(dc.y), static_cast<float>(dc.z)};
      }
      // A camera-following cascade re-renders as the view moves anyway, so it can
      // take the view's LODs; the cached far one can't.
      c.lightVP = renderCascade(station, catalog, meshes, c.target, spec, basis, center,
                                /*cullToRegion=*/near, near ? &camera : nullptr);
    }
    meshes.setShadowCascade(i, c.target.depth.id, c.lightVP, 1.0f / static_cast<float>(spec.size));
    mask |= 1 << i;
  }
  meshes.setShadowCascades(mask);
}

}  // namespace x4sb::editor

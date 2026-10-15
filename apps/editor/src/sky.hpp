#pragma once
#include "nebula_fog.hpp"
#include "planets.hpp"
#include "raylib.h"
#include "sky_domes.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace x4sb::editor {
struct NativeAssets;

struct SceneSun {
  ::Vector3 dir;
  ::Color color;
  float intensity;
  ::Color ambientColor{0, 0, 0, 255};  // the cluster <ambient> light
  float ambientIntensity{0.0f};
};

// Baked-cluster sky, drawn the way the captured game frame draws it
// (docs/2026-09-19-x4-rendering-pipeline-reverse-engineering.md): the sky-sphere
// meshes through their X4 shaders (SkyDomes), the starfield as p1_star billboards,
// the sun's glow=1 lens elements as world-space quads into the HDR scene, and
// every other lens element (rays, ghosts, streaks) as screen-space quads AFTER
// the tonemap — the two halves are draw() and drawLensFlares().
class Sky {
 public:
  // Empty clusterDir -> cluster_11 (harness default pin). `native` must outlive the Sky.
  // The sun is seen from `sectorMacro` inside the cluster (empty = cluster origin).
  explicit Sky(const NativeAssets& native, std::string clusterDir = {},
               std::string sectorMacro = {});
  ~Sky();
  Sky(const Sky&) = delete;
  Sky& operator=(const Sky&) = delete;
  Sky(Sky&&) = delete;
  Sky& operator=(Sky&&) = delete;

  // HDR half: call inside BeginMode3D within the FramePipeline scene.
  void draw(const ::Camera3D& camera);
  // LDR half: call from FramePipeline::end()'s overlay (linear, post-tonemap,
  // 2D drawing context). `width`/`height` = the overlay target size.
  void drawLensFlares(const ::Camera3D& camera, int width, int height);

  // p1_starfield dims its pixel stars up to 15% while the camera moves (X4
  // F_cameraspeed in m/s, F_camerarotationspeed in rad/s).
  void setCameraMotion(float metresPerSecond, float radiansPerSecond);

  // Planet F_time and texture scroll; 0 (the default) is a still frame.
  void setAnimationTime(double seconds) { planets_.setTime(seconds); }

  bool isValid() const { return valid_; }
  [[nodiscard]] std::size_t missingAssets() const {
    return missingAssets_ + domes_.missingAssets() + planets_.missingAssets() +
           fog_.missingAssets();
  }
  // The cluster's volumetric fog; drawScene applies it after the opaque pass.
  [[nodiscard]] NebulaFog& fog() { return fog_; }
  [[nodiscard]] std::size_t domeCount() const { return domes_.count(); }
  [[nodiscard]] std::size_t sunSpriteCount() const { return sunSprites_.size(); }
  SceneSun sun() const { return sun_; }
  // A/B isolation for the sunshot harness: skip the HDR glow half.
  void setGlowEnabled(bool enabled) { glowEnabled_ = enabled; }

  // Swap to another baked cluster ("cluster_NN"). Frame-boundary only (the
  // caller must also EnvCubemap::rebake afterwards, outside FramePipeline).
  void reload(const std::string& clusterDir, const std::string& sectorMacro = {});
  [[nodiscard]] const std::string& clusterDir() const { return clusterDir_; }
  [[nodiscard]] const std::string& sectorMacro() const { return sectorMacro_; }

 private:
  struct Star {
    ::Vector3 dir;
    ::Color color;  // rgb x intensity, sRGB bytes (the shader linearises)
    float range{1.0f};
    float spec{0.0f};  // atlas column offset, 0..0.875
  };

 public:
  // Public only so the file-local mesh loader/emitter in sky.cpp can see it.
  struct SunSpriteGpu {
    ::Texture2D texture{};  // shared atlas; ownership deduped at unload by id
    // The part's lod0 mesh in unit space (x/y -0.5..0.5): X4 draws every lens
    // element as this geometry, so the shape, atlas sub-rect and per-vertex
    // falloff (corona suns are pure vertex colour on a flat white texture)
    // are all here rather than in a quad + rect.
    std::vector<float> xy;
    std::vector<float> uv;
    std::vector<unsigned char> rgb;
    std::vector<unsigned int> index;
    bool screenMode{false};  // size = authored/scale (screen quad) else radians
    float size{0.0f};
    float rotation{0.0f};             // screen-element roll; X4 ignores it for glow elements
    float position{1.0f};             // 1 = at the sun; else along the sun->centre axis
    bool glow{false};                 // HDR world quad (before bloom) vs post-tonemap quad
    ::Vector3 mul{1.0f, 1.0f, 1.0f};  // diffuseStr x linear color_emissive
    bool screenBlend{false};          // SCREEN (One, InvSrcCol) vs ADDITIVE (One, One)
    ::Vector4 grade{1.0f, 1.0f, 1.0f, 0.0f};  // brightness, contrast, saturation, hue deg
  };

 private:
  bool load();  // clusterDir_ from the game archives
  void loadFog(const nlohmann::json& manifestFog, const std::string& cluster);
  void unloadClusterAssets();  // domes/sprites/planets + their textures; keeps shaders
  void applyFallback();        // procedural stars + default sun (no baked sky)
  void drawStars(const ::Camera3D& camera);
  void uploadStars();  // stars_ -> the static billboard buffer drawStars draws
  void drawGlowSprites();

  const NativeAssets& native_;
  std::size_t missingAssets_{};
  std::string clusterDir_;
  std::string sectorMacro_;

  bool valid_{false};
  bool glowEnabled_{true};
  float starMotion_{1.0f};
  SceneSun sun_{};
  ::Texture2D sunGlow_{};  // procedural radial falloff for the no-manifest fallback sun
  ::Texture2D starAtlas_{};
  ::Shader flareShader_{};
  ::Shader starShader_{};
  int locFlareMul_{-1};
  int locFlareGrade_{-1};
  int locStarRight_{-1};
  int locStarUp_{-1};
  int locStarScale_{-1};
  unsigned int starVao_{0};
  unsigned int starVbo_{0};
  int starVertexCount_{0};

  std::vector<Star> stars_;
  SkyDomes domes_;
  std::vector<SunSpriteGpu> sunSprites_;
  PlanetPass planets_;
  NebulaFog fog_;
};

}  // namespace x4sb::editor

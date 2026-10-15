#pragma once
// X4's volumetric nebula fog (docs/2026-10-11-nebula-fog-findings.md): every fog
// sphere adds its view-ray chord, shaped by a triplanar pattern, to a per-medium
// optical path target; a fullscreen composite then dims the scene by the
// transmittance and adds the sun's in-scattering. Ports X4's two passes without
// the near froxel volume and the fog self-shadow.
// Needs a live GL context: construct after InitWindow, destroy before CloseWindow.
#include "raylib.h"
#include "scene_depth.hpp"
#include "x4sb/assetpipe/nebula_fog.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace x4sb::editor {
struct NativeAssets;

class NebulaFog {
 public:
  NebulaFog();
  ~NebulaFog();
  NebulaFog(const NebulaFog&) = delete;
  NebulaFog& operator=(const NebulaFog&) = delete;
  NebulaFog(NebulaFog&&) = delete;
  NebulaFog& operator=(NebulaFog&&) = delete;

  void load(const assetpipe::FogField& field, const NativeAssets& native);
  void unload();
  // Where the station sits in the cluster (X4 axes); load() takes the field's origin.
  void setOrigin(Vec3 clusterPos);
  // Display-space direction toward the sun and X4's V_lightcolor1.
  void setSun(::Vector3 dir, ::Vector3 color);
  void setEnabled(bool enabled) { enabled_ = enabled; }

  [[nodiscard]] std::size_t sphereCount() const { return spheres_.size(); }
  [[nodiscard]] std::size_t mediumCount() const { return media_.size(); }
  [[nodiscard]] std::size_t missingAssets() const { return missingAssets_; }

  // Fogs the bound scene target in place, after the opaque pass: `depth` is its
  // copy, seen through the current rlgl projection and clip planes.
  void apply(const ::Camera3D& camera, const SceneDepthCopy& depth);

 private:
  void uploadInstances();
  void resize(std::array<int, 2> dims);

  std::vector<assetpipe::FogSphere> spheres_;
  std::vector<assetpipe::FogMedium> media_;
  std::vector<::Texture2D> patterns_;  // per medium
  std::vector<int> drawCounts_;        // instances per medium, uploaded in medium order
  Vec3 origin_;
  ::Vector3 sunDir_{0.0F, 1.0F, 0.0F};
  ::Vector3 sunColor_{};
  bool enabled_{true};
  std::size_t missingAssets_{};

  ::Shader pathShader_{};
  ::Shader compositeShader_{};
  unsigned int sphereVao_{0};
  unsigned int sphereVbo_{0};
  unsigned int sphereEbo_{0};
  unsigned int instanceVbo_{0};
  int sphereIndexCount_{0};
  unsigned int fullscreenVao_{0};
  unsigned int pathTex_{0};
  unsigned int pathFbo_{0};
  std::array<int, 2> dims_{};
};

}  // namespace x4sb::editor

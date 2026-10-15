#pragma once
// Directional sun shadow: X4's five cascades (CSM0-4). The four near ones are
// light-aligned cubes ahead of the camera and follow it; the far one (CSM4)
// covers the station at viewing distance and is centred on it, so it only
// re-renders on edits. Render once per frame BEFORE FramePipeline::begin(): each
// cascade owns its own FBO and raylib's EndTextureMode returns to the default
// framebuffer, so nesting it inside the scene target would break the post chain.
#include "mesh_cache.hpp"
#include "raylib.h"
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <array>
#include <cstdint>

namespace x4sb::editor {

class SunShadow {
 public:
  SunShadow();
  ~SunShadow();
  SunShadow(const SunShadow&) = delete;
  SunShadow& operator=(const SunShadow&) = delete;
  SunShadow(SunShadow&&) = delete;
  SunShadow& operator=(SunShadow&&) = delete;

  // Depth passes, then binds every cascade's map + light matrix into `meshes` for
  // this frame's lit draws. `sunDir` points TO the sun, display space. A cascade
  // re-renders only when the station, the sun, the loaded meshes or (near ones)
  // its texel-snapped position changed.
  void render(const Station& station, const ModuleCatalog& catalog, MeshCache& meshes,
              ::Vector3 sunDir, const ::Camera3D& camera);

  static constexpr int kCascades = 5;

 private:
  struct Cascade {
    ::RenderTexture2D target{};  // depth attachment only; texture.id stays 0
    std::uint64_t renderedKey{0};
    ::Matrix lightVP{};
  };
  std::array<Cascade, kCascades> cascades_{};
};

}  // namespace x4sb::editor

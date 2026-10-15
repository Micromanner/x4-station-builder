#include "graphics_settings.hpp"

#include "mesh_cache.hpp"
#include "sky.hpp"

namespace x4sb::editor {

void applyGraphicsSettings(const GraphicsSettings& gfx, MeshCache& meshes, Sky& sky) {
  // lightMask bits: 1 sun diffuse, 2 sun specular, 4 IBL, 8 local lights.
  meshes.setLightMask(gfx.localLights ? 15 : 7);
  meshes.setSsaoEnabled(gfx.ssao);
  meshes.setPomEnabled(gfx.parallax);
  sky.setGlowEnabled(gfx.sunFx);
  sky.fog().setEnabled(gfx.nebulaFog);
}

}  // namespace x4sb::editor

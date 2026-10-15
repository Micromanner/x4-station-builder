#pragma once
// User-facing graphics switches, the backing state for a future settings page. Each
// one turns a feature off without changing how it renders when on, so the defaults
// reproduce the game-matched look. Hotkeys (K, L, P) flip the same fields.

namespace x4sb::editor {

class MeshCache;
class Sky;

struct GraphicsSettings {
  bool animation = true;    // texture scroll + planet motion; off freezes in place
  bool shadows = true;      // sun cascades
  bool localLights = true;  // module point/spot lights
  bool ssao = true;
  bool sunFx = true;  // sun glow quads + lens flares
  bool nebulaFog = true;
  bool parallax = true;      // POM on materials that carry a height map
  bool distanceLod = false;  // collapse far modules to boxes (huge stations)
};

// Pushes the state-style switches into the renderer. The per-frame ones (animation,
// shadows, sunFx flares, distanceLod) are read by the frame loop directly.
void applyGraphicsSettings(const GraphicsSettings& gfx, MeshCache& meshes, Sky& sky);

}  // namespace x4sb::editor

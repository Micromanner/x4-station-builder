#pragma once
// A spherical orbit camera driven by the right mouse button (so left-click stays
// free for placement/selection) and the wheel. Render-side; links raylib.
#include "raylib.h"

namespace x4sb::editor {

class OrbitCamera {
 public:
  OrbitCamera();
  // Apply this frame's input (right-drag orbit, wheel zoom). Call once per frame.
  void update();
  // Recenter on `target` and frame a sphere of the given radius.
  void frame(::Vector3 target, float radius);
  [[nodiscard]] const ::Camera3D& camera() const { return cam_; }

 private:
  void rebuild();  // recompute cam_.position from yaw_/pitch_/distance_/target_

  ::Camera3D cam_{};
  float yaw_{0.785f};        // radians
  float pitch_{0.6f};        // radians (above the horizon)
  float distance_{3000.0f};  // X4 scale: a single large module is ~1500 units
  ::Vector3 target_{0.0f, 0.0f, 0.0f};
};

}  // namespace x4sb::editor

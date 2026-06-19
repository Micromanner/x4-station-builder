#pragma once
// A spherical orbit camera driven by the right mouse button (so left-click stays
// free for placement/selection) and the wheel. Render-side; links raylib.
#include "x4sb/data/math.hpp"

#include "raylib.h"

#include <optional>

namespace x4sb::editor {

class OrbitCamera {
 public:
  OrbitCamera();
  // Apply this frame's input (right-drag orbit, wheel zoom). Call once per frame.
  // `zoomFocus` is the display-space scene point under the cursor (shell hit-test):
  // the wheel dollies toward it; nullopt (empty space) is a plain dolly that leaves
  // the pivot put, so scrolling over the void no longer drifts the view.
  void update(std::optional<Vec3> zoomFocus = std::nullopt);
  // Recenter on `target` and frame a sphere of the given radius.
  void frame(::Vector3 target, float radius);
  [[nodiscard]] const ::Camera3D& camera() const { return cam_; }
  // Current orbit distance (eye<->pivot). The shell uses it as the free-placement
  // standoff baseline so a fresh ghost sits at a view-appropriate depth.
  [[nodiscard]] float distance() const { return distance_; }

 private:
  void rebuild();  // recompute cam_.position from yaw_/pitch_/distance_/target_

  ::Camera3D cam_{};
  float yaw_{0.785f};        // radians
  float pitch_{0.6f};        // radians (above the horizon)
  float distance_{3000.0f};  // X4 scale: a single large module is ~1500 units
  ::Vector3 target_{0.0f, 0.0f, 0.0f};
};

}  // namespace x4sb::editor

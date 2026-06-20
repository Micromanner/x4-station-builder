#include "orbit_camera.hpp"

#include "camera_math.hpp"
#include "modifier_keys.hpp"
#include "raylib_convert.hpp"

#include <algorithm>
#include <cmath>

namespace x4sb::editor {
namespace {
// Wheel-out / frame() dolly limit: how far the eye can pull back from its pivot.
constexpr float kMaxDistance = 17000.0f;
// The cube the pivot (look-at point) may be panned/flown within — comfortably larger
// than the ~20km plot so the eye can still center on any plot point when fully zoomed
// out. With kMaxDistance this caps the eye at ~39km from center.
constexpr float kPivotClamp = 22100.0f;
}  // namespace

OrbitCamera::OrbitCamera() {
  cam_.up = ::Vector3{0.0f, 1.0f, 0.0f};
  cam_.fovy = 60.0f;
  cam_.projection = CAMERA_PERSPECTIVE;
  rebuild();
}

void OrbitCamera::update(std::optional<Vec3> zoomFocus) {
  // Net keyboard fly/yaw, Ctrl-guarded in one place (known-issues 3.10) so Ctrl
  // chords (save/load/undo) never also drift or turn the view.
  const bool ctrl = isCtrlDown();
  const FlyInput fly =
      resolveFlyInput(IsKeyDown(KEY_W), IsKeyDown(KEY_S), IsKeyDown(KEY_A), IsKeyDown(KEY_D),
                      IsKeyDown(KEY_Z), IsKeyDown(KEY_X), IsKeyDown(KEY_Q), IsKeyDown(KEY_E), ctrl);

  if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
    const ::Vector2 d = GetMouseDelta();
    yaw_ -= d.x * 0.005f;
    pitch_ = std::clamp(pitch_ + d.y * 0.005f, -1.5f, 1.5f);
  }
  // Keyboard yaw (Q/E) turns the view at a steady rate, complementing RMB-drag orbit.
  // Q (+1) raises yaw_, matching an RMB drag-left (yaw_ -= d.x), so Q reads as "left".
  if (fly.yaw != 0.0) {
    constexpr float kYawRadPerSec = 1.2f;
    yaw_ += static_cast<float>(fly.yaw) * kYawRadPerSec * GetFrameTime();
  }
  rebuild();  // refresh cam_ so the pan/zoom/fly gestures use this frame's pose

  const bool panning = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
  const float wheel = GetMouseWheelMove();
  const bool flying = fly.forward != 0.0 || fly.strafe != 0.0 || fly.rise != 0.0;
  if (!panning && wheel == 0.0f && !flying) return;  // no movable-pivot gesture this frame

  // The camera basis is invariant under pan and fly (they translate the pivot but
  // never change yaw/pitch), so derive it once and share it across them.
  const CameraBasis basis =
      cameraBasis(toVec3(cam_.target) - toVec3(cam_.position), Vec3{0.0, 1.0, 0.0});

  if (panning) {
    const ::Vector2 d = GetMouseDelta();
    // Screen-space scale so a pixel of drag maps to a constant world distance at
    // the pivot depth, regardless of zoom.
    const double scale = pixelsToWorldAtDepth(
        static_cast<double>(cam_.fovy) * static_cast<double>(DEG2RAD),
        static_cast<double>(GetScreenHeight()), static_cast<double>(distance_));
    target_ = toRl(toVec3(target_) +
                   panOffset(basis, static_cast<double>(d.x), static_cast<double>(d.y), scale));
    rebuild();
  }

  if (wheel != 0.0f) {
    // Symmetric factor so equal in/out notches return to the same distance (the old
    // 1 - wheel*0.1 made 0.9*1.1 = 0.99, a slow drift); pow also handles fractional
    // / high-resolution wheel deltas correctly.
    const double k = std::pow(0.9, static_cast<double>(wheel));
    // Dolly toward the scene point under the cursor when the shell found one; over
    // empty space (focus == pivot) this degrades to a plain dolly that leaves the
    // pivot put, instead of drifting it cursor-ward across the void.
    const Vec3 focus = zoomFocus.value_or(toVec3(target_));
    const ZoomResult z = zoomTowardPoint(toVec3(target_), static_cast<double>(distance_), focus, k,
                                         2.0, static_cast<double>(kMaxDistance));
    target_ = toRl(z.target);
    distance_ = static_cast<float>(z.distance);
    rebuild();
  }

  // WASD bulk fly: steady linear traversal that the screen-plane pan and the
  // non-linear zoom-toward-cursor don't give. W/S = forward/back along the look
  // direction, A/D = strafe, Z/X = world up/down. Speed scales with orbit
  // distance (so it's brisk far out and precise up close) and frame time.
  if (flying) {
    constexpr double kFlyUnitsPerDistPerSec = 1.5;
    const double speed = std::max(static_cast<double>(distance_), 200.0) * kFlyUnitsPerDistPerSec *
                         static_cast<double>(GetFrameTime());
    target_ = toRl(toVec3(target_) +
                   flyOffset(basis, Vec3{0.0, 1.0, 0.0}, fly.forward, fly.strafe, fly.rise, speed));
    rebuild();
  }
}

void OrbitCamera::frame(::Vector3 target, float radius) {
  target_ = target;
  distance_ = std::clamp(radius * 2.5f, 5.0f, kMaxDistance);
  rebuild();
}

void OrbitCamera::rebuild() {
  // Clamp the pivot to the kPivotClamp cube so the eye can reach the plot edges even
  // when fully zoomed out (see the constant for the sizing rationale).
  target_.x = std::clamp(target_.x, -kPivotClamp, kPivotClamp);
  target_.y = std::clamp(target_.y, -kPivotClamp, kPivotClamp);
  target_.z = std::clamp(target_.z, -kPivotClamp, kPivotClamp);

  const float cp = std::cos(pitch_);
  cam_.position = ::Vector3{target_.x + distance_ * cp * std::sin(yaw_),
                            target_.y + distance_ * std::sin(pitch_),
                            target_.z + distance_ * cp * std::cos(yaw_)};
  cam_.target = target_;
}

}  // namespace x4sb::editor

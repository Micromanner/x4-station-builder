#include "orbit_camera.hpp"

#include "camera_math.hpp"
#include "raylib_convert.hpp"

#include <algorithm>
#include <cmath>

namespace x4sb::editor {

OrbitCamera::OrbitCamera() {
  cam_.up = ::Vector3{0.0f, 1.0f, 0.0f};
  cam_.fovy = 45.0f;
  cam_.projection = CAMERA_PERSPECTIVE;
  rebuild();
}

void OrbitCamera::update() {
  if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
    const ::Vector2 d = GetMouseDelta();
    yaw_ -= d.x * 0.005f;
    pitch_ = std::clamp(pitch_ + d.y * 0.005f, -1.5f, 1.5f);
  }
  rebuild();  // refresh cam_ so pan/zoom use this frame's orbit pose

  if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
    const ::Vector2 d = GetMouseDelta();
    const CameraBasis basis = cameraBasis(toVec3(cam_.target) - toVec3(cam_.position),
                                          Vec3{0.0, 1.0, 0.0});
    // Screen-space scale so a pixel of drag maps to a constant world distance at
    // the pivot depth, regardless of zoom.
    constexpr double kDegToRad = 0.017453292519943295;
    const double scale = 2.0 * static_cast<double>(distance_) *
                         std::tan(static_cast<double>(cam_.fovy) * kDegToRad * 0.5) /
                         static_cast<double>(GetScreenHeight());
    const Vec3 off = panOffset(basis, static_cast<double>(d.x), static_cast<double>(d.y), scale);
    target_ = toRl(toVec3(target_) + off);
    rebuild();
  }

  const float wheel = GetMouseWheelMove();
  if (wheel != 0.0f) {
    const ::Ray r = GetScreenToWorldRay(GetMousePosition(), cam_);
    const double k = 1.0 - static_cast<double>(wheel) * 0.1;
    const ZoomResult z = zoomTowardCursor(
        toVec3(target_), static_cast<double>(distance_),
        toVec3(cam_.target) - toVec3(cam_.position),
        toVec3(r.position), toVec3(r.direction), k, 2.0, 1000000.0);
    target_ = toRl(z.target);
    distance_ = static_cast<float>(z.distance);
    rebuild();
  }

  // WASD bulk fly: steady linear traversal that the screen-plane pan and the
  // non-linear zoom-toward-cursor don't give. W/S = forward/back along the look
  // direction, A/D = strafe, E/C = world up/down. Speed scales with orbit
  // distance (so it's brisk far out and precise up close) and frame time.
  const double forward = (IsKeyDown(KEY_W) ? 1.0 : 0.0) - (IsKeyDown(KEY_S) ? 1.0 : 0.0);
  // basis.right is the left-handed "right" (points screen-left for a forward view),
  // so A maps to +right (screen-left) and D to -right (screen-right) to read correctly.
  const double strafe = (IsKeyDown(KEY_A) ? 1.0 : 0.0) - (IsKeyDown(KEY_D) ? 1.0 : 0.0);
  const double rise = (IsKeyDown(KEY_E) ? 1.0 : 0.0) - (IsKeyDown(KEY_C) ? 1.0 : 0.0);
  if (forward != 0.0 || strafe != 0.0 || rise != 0.0) {
    const CameraBasis basis = cameraBasis(toVec3(cam_.target) - toVec3(cam_.position),
                                          Vec3{0.0, 1.0, 0.0});
    constexpr double kFlyUnitsPerDistPerSec = 1.5;
    const double speed = static_cast<double>(distance_) * kFlyUnitsPerDistPerSec *
                         static_cast<double>(GetFrameTime());
    target_ = toRl(toVec3(target_) +
                   flyOffset(basis, Vec3{0.0, 1.0, 0.0}, forward, strafe, rise, speed));
    rebuild();
  }
}

void OrbitCamera::frame(::Vector3 target, float radius) {
  target_ = target;
  distance_ = std::max(radius * 2.5f, 5.0f);
  rebuild();
}

void OrbitCamera::rebuild() {
  const float cp = std::cos(pitch_);
  cam_.position = ::Vector3{target_.x + distance_ * cp * std::sin(yaw_),
                            target_.y + distance_ * std::sin(pitch_),
                            target_.z + distance_ * cp * std::cos(yaw_)};
  cam_.target = target_;
}

}  // namespace x4sb::editor

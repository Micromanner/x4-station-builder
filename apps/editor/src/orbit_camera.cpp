#include "orbit_camera.hpp"

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
  const float wheel = GetMouseWheelMove();
  if (wheel != 0.0f) {
    distance_ = std::clamp(distance_ * (1.0f - wheel * 0.1f), 1.0f, 1000000.0f);
  }
  rebuild();
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

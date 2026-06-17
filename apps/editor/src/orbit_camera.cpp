#include "orbit_camera.hpp"

#include "camera_math.hpp"
#include "raylib_convert.hpp"

#include <algorithm>
#include <cmath>

namespace x4sb::editor {

OrbitCamera::OrbitCamera() {
  cam_.up = ::Vector3{0.0f, 1.0f, 0.0f};
  cam_.fovy = 60.0f;
  cam_.projection = CAMERA_PERSPECTIVE;
  rebuild();
}

void OrbitCamera::update() {
  if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
    const ::Vector2 d = GetMouseDelta();
    yaw_ -= d.x * 0.005f;
    pitch_ = std::clamp(pitch_ + d.y * 0.005f, -1.5f, 1.5f);
  }
  rebuild();  // refresh cam_ so the pan/zoom/fly gestures use this frame's pose

  // WASD net inputs (W-S, A-D, E-C) for the bulk fly below. basis.right is the
  // left-handed "right" (points screen-left for a forward view), so A maps to
  // +right (screen-left) and D to -right (screen-right) to read correctly.
  auto axis = [](int pos, int neg) {
    return (IsKeyDown(pos) ? 1.0 : 0.0) - (IsKeyDown(neg) ? 1.0 : 0.0);
  };
  const double flyFwd = axis(KEY_W, KEY_S);
  const double flyStrafe = axis(KEY_A, KEY_D);
  const double flyRise = axis(KEY_E, KEY_C);

  const bool panning = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
  const float wheel = GetMouseWheelMove();
  const bool flying = flyFwd != 0.0 || flyStrafe != 0.0 || flyRise != 0.0;
  if (!panning && wheel == 0.0f && !flying) return;  // no movable-pivot gesture this frame

  // The camera basis is invariant under pan/zoom/fly (they translate the pivot
  // but never change yaw/pitch), so derive it once and share it across them.
  const CameraBasis basis =
      cameraBasis(toVec3(cam_.target) - toVec3(cam_.position), Vec3{0.0, 1.0, 0.0});

  if (panning) {
    const ::Vector2 d = GetMouseDelta();
    // Screen-space scale so a pixel of drag maps to a constant world distance at
    // the pivot depth, regardless of zoom.
    const double scale =
        pixelsToWorldAtDepth(static_cast<double>(cam_.fovy) * static_cast<double>(DEG2RAD),
                             static_cast<double>(GetScreenHeight()), static_cast<double>(distance_));
    target_ = toRl(toVec3(target_) +
                   panOffset(basis, static_cast<double>(d.x), static_cast<double>(d.y), scale));
    rebuild();
  }

  if (wheel != 0.0f) {
    const ::Ray r = GetScreenToWorldRay(GetMousePosition(), cam_);
    const double k = 1.0 - static_cast<double>(wheel) * 0.1;
    const ZoomResult z =
        zoomTowardCursor(toVec3(target_), static_cast<double>(distance_), basis.forward,
                         toVec3(r.position), toVec3(r.direction), k, 2.0, 40000.0);
    target_ = toRl(z.target);
    distance_ = static_cast<float>(z.distance);
    rebuild();
  }

  // WASD bulk fly: steady linear traversal that the screen-plane pan and the
  // non-linear zoom-toward-cursor don't give. W/S = forward/back along the look
  // direction, A/D = strafe, E/C = world up/down. Speed scales with orbit
  // distance (so it's brisk far out and precise up close) and frame time.
  if (flying) {
    constexpr double kFlyUnitsPerDistPerSec = 1.5;
    const double speed = std::max(static_cast<double>(distance_), 200.0) * kFlyUnitsPerDistPerSec *
                         static_cast<double>(GetFrameTime());
    target_ = toRl(toVec3(target_) +
                   flyOffset(basis, Vec3{0.0, 1.0, 0.0}, flyFwd, flyStrafe, flyRise, speed));
    rebuild();
  }
}

void OrbitCamera::frame(::Vector3 target, float radius) {
  target_ = target;
  distance_ = std::clamp(radius * 2.5f, 5.0f, 40000.0f);
  rebuild();
}

void OrbitCamera::rebuild() {
  // Clamp movement target to +/- 52km (12km box margin + 40km max zoom) so that
  // the camera position can fly anywhere within the plot even when fully zoomed out.
  target_.x = std::clamp(target_.x, -52000.0f, 52000.0f);
  target_.y = std::clamp(target_.y, -52000.0f, 52000.0f);
  target_.z = std::clamp(target_.z, -52000.0f, 52000.0f);

  const float cp = std::cos(pitch_);
  cam_.position = ::Vector3{target_.x + distance_ * cp * std::sin(yaw_),
                            target_.y + distance_ * std::sin(pitch_),
                            target_.z + distance_ * cp * std::cos(yaw_)};
  cam_.target = target_;
}

}  // namespace x4sb::editor

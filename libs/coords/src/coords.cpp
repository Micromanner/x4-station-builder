#include "x4sb/coords/coords.hpp"

#include <algorithm>
#include <cmath>

namespace x4sb {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

Quat axisAngle(double x, double y, double z, double rad) {
  const double h = rad * 0.5;
  const double s = std::sin(h);
  return {std::cos(h), x * s, y * s, z * s};
}

}  // namespace

// Position is X4-native end to end — see the header. Identity, but funnelled here
// so the convention has exactly one home.
Vec3 appToX4(Vec3 v) { return v; }
Vec3 x4ToApp(Vec3 v) { return v; }
Transform appToX4(const Transform& t) { return t; }
Transform x4ToApp(const Transform& t) { return t; }

// Intrinsic Y-X-Z: q = qYaw(Y) * qPitch(X) * qRoll(Z).
Quat quatFromX4Euler(double yawDeg, double pitchDeg, double rollDeg) {
  const Quat qy = axisAngle(0, 1, 0, yawDeg * kDegToRad);
  const Quat qx = axisAngle(1, 0, 0, pitchDeg * kDegToRad);
  const Quat qz = axisAngle(0, 0, 1, rollDeg * kDegToRad);
  return qy * qx * qz;
}

// Inverse of quatFromX4Euler: extract Y-X-Z angles from the rotation matrix of q.
void x4EulerFromQuat(const Quat& q, double& yawDeg, double& pitchDeg, double& rollDeg) {
  const double w = q.w, x = q.x, y = q.y, z = q.z;
  // Rotation-matrix entries needed for the Y-X-Z extraction (v' = R v).
  const double r12 = 2.0 * (y * z - w * x);
  const double r02 = 2.0 * (x * z + w * y);
  const double r22 = 1.0 - 2.0 * (x * x + y * y);
  const double r10 = 2.0 * (x * y + w * z);
  const double r11 = 1.0 - 2.0 * (x * x + z * z);

  const double sinPitch = std::clamp(-r12, -1.0, 1.0);
  const double pitch = std::asin(sinPitch);
  double yaw, roll;
  if (std::abs(sinPitch) < 0.9999995) {
    yaw = std::atan2(r02, r22);
    roll = std::atan2(r10, r11);
  } else {
    // Gimbal lock (pitch = +/-90 deg): fold roll into yaw.
    const double r20 = 2.0 * (x * z - w * y);
    const double r00 = 1.0 - 2.0 * (y * y + z * z);
    yaw = std::atan2(-r20, r00);
    roll = 0.0;
  }
  yawDeg = yaw * kRadToDeg;
  pitchDeg = pitch * kRadToDeg;
  rollDeg = roll * kRadToDeg;
}

}  // namespace x4sb

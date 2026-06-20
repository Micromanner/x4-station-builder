#pragma once
// x4sb (double, X4 native space) <-> raylib (float) conversions. The scene is
// flipped as a whole via rlScalef(1,1,-1) in the renderer, so geometry is built
// in NATIVE X4 coords here — no per-vector flip. raylib's Quaternion is laid out
// {x,y,z,w}; x4sb's Quat is {w,x,y,z}.
#include "raylib.h"
#include "raymath.h"
#include "x4sb/data/math.hpp"

namespace x4sb::editor {

[[nodiscard]] inline ::Vector3 toRl(Vec3 v) {
  return ::Vector3{static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

[[nodiscard]] inline Vec3 toVec3(::Vector3 v) {
  return Vec3{static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

// Model matrix (rotation then translation) from an x4sb rigid Transform.
[[nodiscard]] inline ::Matrix toRlMatrix(const Transform& t) {
  const ::Quaternion q{static_cast<float>(t.rotation.x), static_cast<float>(t.rotation.y),
                       static_cast<float>(t.rotation.z), static_cast<float>(t.rotation.w)};
  const ::Matrix rot = QuaternionToMatrix(q);
  const ::Matrix trans =
      MatrixTranslate(static_cast<float>(t.position.x), static_cast<float>(t.position.y),
                      static_cast<float>(t.position.z));
  return MatrixMultiply(rot, trans);
}

}  // namespace x4sb::editor

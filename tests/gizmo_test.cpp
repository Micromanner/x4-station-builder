#include "x4sb/editorcore/gizmo.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

TEST_CASE("gizmoPick: a ray down an axis selects that axis") {
  const GizmoModel g = gizmoModel(Vec3{0, 0, 0}, 1.0);  // axisLength 1
  // Ray from above the X axis at x=0.5 pointing down hits (0.5,0,0): on +X.
  const auto h = gizmoPick(g, Vec3{0.5, 1, 0}, Vec3{0, -1, 0});
  REQUIRE(h.has_value());
  CHECK(*h == GizmoHandle::AxisX);
}

TEST_CASE("gizmoPick: a ray into a plane quad selects that plane") {
  const GizmoModel g = gizmoModel(Vec3{0, 0, 0}, 1.0);  // planeSize 0.3
  // (0.15,0,0.15) is inside the XZ (PlaneZX) quad and >axisPickRadius from any axis.
  const auto h = gizmoPick(g, Vec3{0.15, 1, 0.15}, Vec3{0, -1, 0});
  REQUIRE(h.has_value());
  CHECK(*h == GizmoHandle::PlaneZX);
}

TEST_CASE("gizmoPick: a ray missing every handle returns none") {
  const GizmoModel g = gizmoModel(Vec3{0, 0, 0}, 1.0);
  // Far from the origin and pointing away.
  CHECK_FALSE(gizmoPick(g, Vec3{50, 50, 50}, Vec3{0, 1, 0}).has_value());
}

TEST_CASE("gizmo axis/plane accessors") {
  CHECK(gizmoIsAxis(GizmoHandle::AxisY));
  CHECK_FALSE(gizmoIsAxis(GizmoHandle::PlaneXY));
  CHECK(gizmoAxisDir(GizmoHandle::AxisZ).z == doctest::Approx(1));
  CHECK(gizmoPlaneNormal(GizmoHandle::PlaneXY).z == doctest::Approx(1));
}

TEST_CASE("gizmoDragDelta: axis drag moves only along that axis") {
  // Drag along +X: rays straight down at x=2 (start) then x=5 (current).
  const Vec3 delta = gizmoDragDelta(GizmoHandle::AxisX, Vec3{0, 0, 0}, Vec3{2, 5, 0},
                                    Vec3{0, -1, 0}, Vec3{5, 5, 0}, Vec3{0, -1, 0});
  CHECK(delta.x == doctest::Approx(3));
  CHECK(delta.y == doctest::Approx(0));
  CHECK(delta.z == doctest::Approx(0));
}

TEST_CASE("gizmoDragDelta: plane drag stays in the plane") {
  // Drag in the XZ plane (PlaneZX, normal +Y): (1,*,1) -> (4,*,3).
  const Vec3 delta = gizmoDragDelta(GizmoHandle::PlaneZX, Vec3{0, 0, 0}, Vec3{1, 5, 1},
                                    Vec3{0, -1, 0}, Vec3{4, 5, 3}, Vec3{0, -1, 0});
  CHECK(delta.x == doctest::Approx(3));
  CHECK(delta.y == doctest::Approx(0));
  CHECK(delta.z == doctest::Approx(2));
}

TEST_CASE("gizmoIsRotation / gizmoAxisDir cover the rotation handles") {
  CHECK(gizmoIsRotation(GizmoHandle::RotY));
  CHECK_FALSE(gizmoIsRotation(GizmoHandle::AxisY));
  CHECK_FALSE(gizmoIsRotation(GizmoHandle::PlaneXY));
  CHECK(gizmoAxisDir(GizmoHandle::RotX).x == doctest::Approx(1));
  CHECK(gizmoAxisDir(GizmoHandle::RotZ).z == doctest::Approx(1));
}

TEST_CASE("gizmoPick: a ray onto a rotation ring selects that ring") {
  const GizmoModel g = gizmoModel(Vec3{0, 0, 0}, 1.0);  // ringRadius 1
  // A point on the RotY ring (XZ plane, r=1) away from any axis crossing, so it
  // can't be mistaken for an axis handle.
  const double s = 0.70710678;
  const auto h = gizmoPick(g, Vec3{s, 1, s}, Vec3{0, -1, 0});
  REQUIRE(h.has_value());
  CHECK(*h == GizmoHandle::RotY);
}

TEST_CASE("gizmoScale: tracks the module size between the screen-relative clamps") {
  // dist 100 -> floor = 100*0.06 = 6, ceiling = 100*0.7 = 70; factor k = 1.3.
  const double dist = 100.0;
  // A mid-size module (radius 20 -> 26) lands between the clamps: gizmo tracks it.
  CHECK(gizmoScale(20.0, dist) == doctest::Approx(26.0));
  // A tiny module (radius 2 -> 2.6) would be sub-pixel: clamped UP to the floor.
  CHECK(gizmoScale(2.0, dist) == doctest::Approx(6.0));
  // A huge module (radius 100 -> 130) would fill the screen: clamped DOWN to the ceiling.
  CHECK(gizmoScale(100.0, dist) == doctest::Approx(70.0));
}

TEST_CASE("gizmoDragRotation: signed angle swept about the ring axis") {
  // RotY ring (axis +Y): start hit at +X, current hit at +Z. About +Y, +X->+Z is
  // -90 deg (right-hand rule sends +X toward -Z for a positive angle).
  const double a = gizmoDragRotation(GizmoHandle::RotY, Vec3{0, 0, 0}, Vec3{1, 5, 0},
                                     Vec3{0, -1, 0}, Vec3{0, 5, 1}, Vec3{0, -1, 0});
  CHECK(a == doctest::Approx(-1.5707963267948966).epsilon(1e-9));
}

#include "camera_math.hpp"

#include "doctest/doctest.h"

#include <cmath>

using x4sb::Vec3;
using namespace x4sb::editor;

namespace {
bool vclose(Vec3 a, Vec3 b, double eps = 1e-9) {
  return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
}
}  // namespace

TEST_CASE("normalized returns a unit vector and zero for a degenerate input") {
  const Vec3 n = normalized(Vec3{0, 0, 5});
  CHECK(vclose(n, Vec3{0, 0, 1}));
  CHECK(vclose(normalized(Vec3{0, 0, 0}), Vec3{0, 0, 0}));
}

TEST_CASE("cameraBasis is orthonormal and right-handed for a forward of -Z") {
  const CameraBasis b = cameraBasis(Vec3{0, 0, -1}, Vec3{0, 1, 0});
  CHECK(vclose(b.forward, Vec3{0, 0, -1}));
  CHECK(std::abs(length(b.right) - 1.0) < 1e-9);
  CHECK(std::abs(length(b.up) - 1.0) < 1e-9);
  CHECK(std::abs(dot(b.right, b.up)) < 1e-9);
  CHECK(std::abs(dot(b.right, b.forward)) < 1e-9);
  // right = normalized(cross(worldUp, forward)); for fwd -Z, up +Y this is -X.
  CHECK(vclose(b.right, Vec3{-1, 0, 0}));
}

TEST_CASE("panOffset slides the pivot opposite both drag axes (view tracks the cursor)") {
  const CameraBasis b{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, -1}};
  // drag right (dx>0) => target slides -X; drag down (dy>0) => target slides -Y.
  CHECK(vclose(panOffset(b, 10.0, 0.0, 2.0), Vec3{-20, 0, 0}));
  CHECK(vclose(panOffset(b, 0.0, 10.0, 2.0), Vec3{0, -20, 0}));
}

TEST_CASE("pixelsToWorldAtDepth is the world span per pixel at the pivot depth") {
  // 90deg vertical FOV => tan(45deg)=1, so span = 2*distance/height.
  const double halfPi = 1.5707963267948966;
  CHECK(pixelsToWorldAtDepth(halfPi, 200.0, 100.0) == doctest::Approx(1.0));
  // Linear in distance: twice as far => twice the world per pixel.
  CHECK(pixelsToWorldAtDepth(halfPi, 200.0, 200.0) == doctest::Approx(2.0));
}

TEST_CASE("flyOffset moves along look/right/world-up and normalizes diagonals") {
  const CameraBasis b{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, -1}};
  const Vec3 up{0, 1, 0};
  // W (forward=+1) follows basis.forward (-Z); D (strafe=+1) follows right (+X);
  // E (rise=+1) follows world up (+Y).
  CHECK(vclose(flyOffset(b, up, 1.0, 0.0, 0.0, 10.0), Vec3{0, 0, -10}));
  CHECK(vclose(flyOffset(b, up, 0.0, 1.0, 0.0, 10.0), Vec3{10, 0, 0}));
  CHECK(vclose(flyOffset(b, up, 0.0, 0.0, 1.0, 10.0), Vec3{0, 10, 0}));
  // Opposite keys cancel; no input => no motion.
  CHECK(vclose(flyOffset(b, up, 0.0, 0.0, 0.0, 10.0), Vec3{0, 0, 0}));
  // Diagonal (forward+strafe) is normalized, so its magnitude is still `speed`.
  CHECK(std::abs(length(flyOffset(b, up, 1.0, 1.0, 0.0, 10.0)) - 10.0) < 1e-9);
}

TEST_CASE("zoomTowardCursor keeps the focal point fixed and clamps distance") {
  // Camera at origin looking down -Z, pivot 100 ahead. Cursor ray hits the pivot
  // plane (z=-100) at x=10 (a ray angled in +X).
  const Vec3 target{0, 0, -100};
  const Vec3 origin{0, 0, 0};
  const Vec3 dir = normalized(Vec3{10, 0, -100});  // crosses z=-100 at x=10
  const ZoomResult r = zoomTowardCursor(target, 100.0, Vec3{0, 0, -1}, origin, dir, 0.5, 2.0,
                                        1000000.0);
  CHECK(r.distance == doctest::Approx(50.0));
  // Focal point P=(10,0,-100); newTarget = P + (target-P)*0.5 = (5,0,-100).
  CHECK(vclose(r.target, Vec3{5, 0, -100}, 1e-6));
}

TEST_CASE("zoomTowardCursor clamps to maxDistance and stops pivot migration there") {
  const Vec3 target{0, 0, -100};
  const Vec3 dir = normalized(Vec3{10, 0, -100});
  // k=2 (zoom out) but already at the max => distance pinned, pivot unmoved.
  const ZoomResult r = zoomTowardCursor(target, 100.0, Vec3{0, 0, -1}, Vec3{0, 0, 0}, dir, 2.0, 2.0,
                                        100.0);
  CHECK(r.distance == doctest::Approx(100.0));
  CHECK(vclose(r.target, target, 1e-6));
}

TEST_CASE("zoomTowardCursor falls back to a plain dolly at a grazing angle") {
  const Vec3 target{0, 0, -100};
  // Ray nearly parallel to the view plane (dir ~perpendicular to forward -Z).
  const Vec3 dir = normalized(Vec3{1, 0, 0});
  const ZoomResult r = zoomTowardCursor(target, 100.0, Vec3{0, 0, -1}, Vec3{0, 0, 0}, dir, 0.5, 2.0,
                                        1000000.0);
  CHECK(r.distance == doctest::Approx(50.0));
  CHECK(vclose(r.target, target, 1e-6));  // pivot unchanged on the grazing fallback
}

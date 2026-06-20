#pragma once
// Render-free translate-gizmo geometry (spec §4). Handle extents come from the
// selected module's origin and a renderer-supplied on-screen scale; the same
// model drives both ray hit-testing here and drawing in apps/editor. Pure x4sb
// math; never includes raylib, so it tests under the `core` preset.
#include "x4sb/data/math.hpp"

#include <limits>
#include <optional>

namespace x4sb {

enum class GizmoHandle { AxisX, AxisY, AxisZ, PlaneXY, PlaneYZ, PlaneZX, RotX, RotY, RotZ, Center };

// Which handle set the gizmo currently shows / picks. Modes are mutually exclusive
// to declutter the viewport (spec §4.2): Translate = arrows + planes + center,
// Rotate = rings.
enum class GizmoMode { Translate, Rotate };

// Handle extents in world units. `scale` is the world length the renderer wants
// the axis handles to span at the module's current camera distance (constant
// on-screen size).
struct GizmoModel {
  Vec3 origin{};
  double axisLength{1.0};
  double planeSize{0.3};          // square plane-quad side, from the origin
  double axisPickRadius{0.08};    // ray-to-axis distance threshold; also the ring band
  double ringRadius{1.0};         // rotation-ring radius (circumscribes the arrows)
  double centerPickRadius{0.15};  // sphere radius at the origin
};

[[nodiscard]] GizmoModel gizmoModel(Vec3 origin, double scale);

// Handle length for the gizmo: constant on-screen size via the camera-space DEPTH to the
// module (depth*factor projects to the same pixels at every zoom). Floored at a fraction of
// the module's OWN eye distance — not the orbit distance, which made the size track the zoom
// pivot — so it can't collapse as the part drifts off-axis; capped at `maxScale` so a
// far/zoomed-out part isn't swallowed (past the cap it shrinks WITH the world). Feeds BOTH
// the draw and the ray hit-test. Spec §4.1.
[[nodiscard]] double gizmoScale(double depthToModule, double distanceToModule,
                                double maxScale = std::numeric_limits<double>::infinity());

// Which in-mode handle (if any) the X4-space ray picks. Plane quads take priority
// over axes. Handles not in `mode` are ignored entirely (so an out-of-mode handle
// can never occlude an in-mode one). Spec §4.2.
[[nodiscard]] std::optional<GizmoHandle> gizmoPick(const GizmoModel& g, Vec3 rayOrigin, Vec3 rayDir,
                                                   GizmoMode mode);

// True if `h` is shown/pickable in `mode`: Rotate => the rings; Translate => axes,
// planes, and the center handle.
[[nodiscard]] bool gizmoHandleInMode(GizmoHandle h, GizmoMode mode);

[[nodiscard]] bool gizmoIsAxis(GizmoHandle h);       // AxisX/Y/Z
[[nodiscard]] bool gizmoIsRotation(GizmoHandle h);   // RotX/Y/Z
[[nodiscard]] Vec3 gizmoAxisDir(GizmoHandle h);      // AxisX/RotX -> +X, etc.
[[nodiscard]] Vec3 gizmoPlaneNormal(GizmoHandle h);  // PlaneXY -> +Z, etc.

// World translation from dragging `handle` between two rays. Axis handles
// constrain to the handle's line through `origin`; plane handles to its plane.
// `origin` is the gizmo origin captured at drag start. Returns the delta to add
// to the drag-start position.
[[nodiscard]] Vec3 gizmoDragDelta(GizmoHandle handle, Vec3 origin, Vec3 startRayOrigin,
                                  Vec3 startRayDir, Vec3 curRayOrigin, Vec3 curRayDir);

// Signed angle (radians) swept about a rotation handle's axis by dragging between
// two rays — the angle from the start hit to the current hit, measured in the ring
// plane through `origin`. 0 if either ray is parallel to that plane.
[[nodiscard]] double gizmoDragRotation(GizmoHandle handle, Vec3 origin, Vec3 startRayOrigin,
                                       Vec3 startRayDir, Vec3 curRayOrigin, Vec3 curRayDir);

}  // namespace x4sb

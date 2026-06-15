#pragma once
// App <-> X4 coordinate & rotation conversion (spec §8). This is the ONLY place
// the editor/X4 convention is applied — every plan I/O path funnels through here.
//
// Convention (decided from real game data, 2026-06-15): the app works in X4's
// NATIVE coordinate space. Module connector geometry (parsed from component XML)
// and construction-plan positions are authored in the *same* X4 space, so the
// snap engine, the document model, and plan I/O all share it directly and the
// position conversion is the identity. (Swapping axes on only the plan side —
// the previous placeholder — would desync plan positions from connector geometry
// and break snapping.) X4 is left-handed / Y-up; the left->right-handed flip for
// display is the renderer's job in apps/editor, NOT this I/O boundary.
#include "x4sb/data/math.hpp"

namespace x4sb {

// Position/transform conversion at the plan boundary. Identity today; kept as the
// single, documented choke point so an axis correction (should in-game testing
// ever require one) lives in exactly one place.
Vec3 appToX4(Vec3 v);
Vec3 x4ToApp(Vec3 v);
Transform appToX4(const Transform& t);
Transform x4ToApp(const Transform& t);

// X4 construction plans express rotation as yaw/pitch/roll in DEGREES (intrinsic
// Y-X-Z: yaw about Y/up, pitch about X, roll about Z), while the document model
// uses quaternions. These bridge the two and are exact inverses, so a plan's
// rotation round-trips losslessly. Component XML already gives quaternions, so it
// needs no Euler conversion.
Quat quatFromX4Euler(double yawDeg, double pitchDeg, double rollDeg);
void x4EulerFromQuat(const Quat& q, double& yawDeg, double& pitchDeg, double& rollDeg);

}  // namespace x4sb

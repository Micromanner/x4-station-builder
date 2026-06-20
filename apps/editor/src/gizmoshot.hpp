#pragma once
// Hidden visual harness: render the translate/rotate gizmo on a selected module —
// idle, mid translate-drag, and mid rotate-drag — and screenshot each. Lets a
// headless run eyeball the gizmo's look (ring style, redundant lines, on-screen
// scale) and the LIVE drag preview without a human driving the editor. Owns its
// own InitWindow/CloseWindow; dispatched from main before the interactive window.
#include "raylib.h"

#include <string>

namespace x4sb::editor {

int runGizmoShot(const std::string& outPrefix);

// Ground-truth diagnostic: replays a real zoom gesture through the editor's own orbit
// camera + gizmoScaleFor + raylib GetWorldToScreen and prints the gizmo's true pixel
// size per step. No screenshots — pure measurement of "does the handle stay constant?".
int runGizmoSweep();

// Build the editor's orbit ::Camera3D for a given pose (mirrors OrbitCamera::rebuild
// + the fovy=60 perspective set in OrbitCamera's ctor).
[[nodiscard]] ::Camera3D orbitCam(::Vector3 target, double dist, double yaw, double pitch);

}  // namespace x4sb::editor

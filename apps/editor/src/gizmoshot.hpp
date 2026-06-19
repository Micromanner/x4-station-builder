#pragma once
// Hidden visual harness: render the translate/rotate gizmo on a selected module —
// idle, mid translate-drag, and mid rotate-drag — and screenshot each. Lets a
// headless run eyeball the gizmo's look (ring style, redundant lines, on-screen
// scale) and the LIVE drag preview without a human driving the editor. Owns its
// own InitWindow/CloseWindow; dispatched from main before the interactive window.
#include <string>

namespace x4sb::editor {

int runGizmoShot(const std::string& outPrefix);

// Ground-truth diagnostic: replays a real zoom gesture through the editor's own orbit
// camera + gizmoScaleFor + raylib GetWorldToScreen and prints the gizmo's true pixel
// size per step. No screenshots — pure measurement of "does the handle stay constant?".
int runGizmoSweep();

}  // namespace x4sb::editor

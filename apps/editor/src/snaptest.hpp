#pragma once
// Visual-regression harness (hidden `--snaptest <outPrefix>` flag). Places a
// snapped module pair through the REAL snap math (makeSnapPlacement -> kMate) and
// the REAL render path (drawScene), screenshots a mesh shot and a box shot, prints
// the resolved transforms, then exits. Lets a human eyeball the snap convention +
// handedness without driving the interactive editor. apps/editor only (links GL).
#include <string>

namespace x4sb::editor {

// Run the harness and return a process exit code (0 ok, 1 on error). Owns its own
// InitWindow/MeshCache/CloseWindow; do NOT call after the interactive path has
// created a window. Writes "<outPrefix>_mesh.png" and "<outPrefix>_box.png".
[[nodiscard]] int runSnapTest(const std::string& outPrefix);

}  // namespace x4sb::editor

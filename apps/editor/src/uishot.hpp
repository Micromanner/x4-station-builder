#pragma once
// Hidden visual-regression harness: `--uishot <outPrefix>` renders the floating
// top bar over a small sample station, screenshots it (idle + Save-hovered), and
// exits. Mirrors --gizmoshot; the project verifies raylib UI code this way (build
// gate + screenshot + human visual pass), since it can't be tested headlessly.
#include <string>

namespace x4sb::editor {

struct NativeAssets;
int runUiShot(const NativeAssets& native, const std::string& outPrefix);
}  // namespace x4sb::editor

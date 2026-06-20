#pragma once
// Modifier-key queries: a held modifier is its left OR right key. Centralized so
// the `IsKeyDown(KEY_LEFT_x) || IsKeyDown(KEY_RIGHT_x)` idiom isn't copy-pasted
// across the input/camera/plan-io frame handlers.
#include "raylib.h"

namespace x4sb::editor {

[[nodiscard]] inline bool isCtrlDown() {
  return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

[[nodiscard]] inline bool isShiftDown() {
  return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

[[nodiscard]] inline bool isAltDown() {
  return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
}

}  // namespace x4sb::editor

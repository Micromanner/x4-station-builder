#pragma once
// Maps this frame's mouse + keyboard into EditorState mutations. Owns the
// display->X4 ray flip (parent §4/§8); EditorState only ever sees X4-space rays.
#include "x4sb/editorcore/editor_state.hpp"

#include "raylib.h"

namespace x4sb::editor {

// Build the mouse-ray in X4 space (raylib ray flipped by negating Z).
void mouseRayX4(const ::Camera3D& camera, Vec3& originOut, Vec3& dirOut);

// Apply keyboard shortcuts (cycle/filter/delete/undo/redo) for this frame.
void handleKeys(EditorState& state);

}  // namespace x4sb::editor

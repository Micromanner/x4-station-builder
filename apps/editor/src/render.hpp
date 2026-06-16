#pragma once
// Draws EditorState with raylib. All scene geometry is emitted in native X4
// coords inside a single global scale(1,1,-1) flip (parent §4); the 2D HUD is
// drawn outside Mode3D.
#include "x4sb/editorcore/editor_state.hpp"

#include "raylib.h"

namespace x4sb::editor {

// Draw the 3D scene (build box, modules, connector markers, ghost, and per-module
// orientation gizmos when showGizmos). BeginMode3D/EndMode3D are handled inside.
void drawScene(const EditorState& state, const ::Camera3D& camera, bool showGizmos);

// Draw the 2D HUD overlay (active module, filter, counts, undo state, controls).
void drawHud(const EditorState& state, int screenWidth, int screenHeight, bool showGizmos);

}  // namespace x4sb::editor

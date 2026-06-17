#pragma once
// Maps this frame's mouse + keyboard into EditorState mutations. Owns the
// display->X4 ray flip (parent §4/§8); EditorState only ever sees X4-space rays.
#include "x4sb/editorcore/editor_state.hpp"
#include "x4sb/editorcore/gizmo.hpp"

#include "raylib.h"

namespace x4sb::editor {

// Build the mouse-ray in X4 space (raylib ray flipped by negating Z).
void mouseRayX4(const ::Camera3D& camera, Vec3& originOut, Vec3& dirOut);

// Apply keyboard shortcuts (cycle/filter/delete/undo/redo/rotate) for this frame.
void handleKeys(EditorState& state);

// World-space gizmo handle length: tracks the selected module's AABB size (clamped
// to a screen-relative min/max via editorcore's gizmoScale) so the gizmo looks
// anchored to the module instead of growing on zoom-out. Sane default if unselected.
[[nodiscard]] double gizmoScaleFor(const ::Camera3D& camera, const EditorState& state);

// Per-frame mouse handling: ghost preview, gizmo grab/drag/commit, and click
// select/place. Owns the display->X4 ray flip.
void handleMouse(EditorState& state, const ::Camera3D& camera);

}  // namespace x4sb::editor

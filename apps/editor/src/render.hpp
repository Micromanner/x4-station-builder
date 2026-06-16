#pragma once
// Draws EditorState with raylib. All scene geometry is emitted in native X4
// coords inside a single global scale(1,1,-1) flip (parent §4); the 2D HUD is
// drawn outside Mode3D.
#include "mesh_cache.hpp"

#include "x4sb/editorcore/editor_state.hpp"

#include "raylib.h"

#include <string>

namespace x4sb::editor {

// Draw the 3D scene (build box, modules, connector markers, ghost, and per-module
// orientation gizmos when showGizmos). When showMeshes, each module is drawn as a
// glTF wireframe (via `meshes`), falling back to its AABB box if no mesh loads;
// when false, always the box. BeginMode3D/EndMode3D are handled inside.
void drawScene(const EditorState& state, const ::Camera3D& camera, MeshCache& meshes,
               bool showGizmos, bool showMeshes);

// Same scene from a raw Station + catalog (no selection, no ghost) — used by the
// --snaptest visual-regression harness so it renders through the identical path.
void drawScene(const Station& station, const ModuleCatalog& catalog, const ::Camera3D& camera,
               MeshCache& meshes, bool showGizmos, bool showMeshes);

// Draw the 2D HUD overlay (active module, filter, counts, undo state, controls).
void drawHud(const EditorState& state, int screenWidth, int screenHeight, bool showGizmos);

// Draw a transient status line (plan save/load result) bottom-left, over the HUD.
void drawToast(const std::string& message, int screenHeight);

}  // namespace x4sb::editor

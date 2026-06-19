#pragma once
// Draws EditorState with raylib. All scene geometry is emitted in native X4
// coords inside a single global scale(1,1,-1) flip (parent §4); the 2D HUD is
// drawn outside Mode3D.
#include "mesh_cache.hpp"

#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/editor_state.hpp"

#include "raylib.h"

#include <string>

namespace x4sb::editor {

// Draw the 3D scene (build box, modules, connector markers, ghost, and per-module
// orientation gizmos when showGizmos). When showMeshes, each module is drawn as a
// glTF wireframe (via `meshes`), falling back to its AABB box if no mesh loads;
// when false, always the box. BeginMode3D/EndMode3D are handled inside.
// `lodEnabled` keeps the distance-LOD box collapse (cheap on huge stations); when
// false every visible module draws as a full mesh (frustum culling still applies) —
// the editor default, since a real station is meant to be seen, and mesh instancing
// makes "all meshes" affordable.
void drawScene(const EditorState& state, const ::Camera3D& camera, MeshCache& meshes,
               bool showGizmos, bool showMeshes, bool lodEnabled = true);

// Same scene from a raw Station + catalog (no selection, no ghost) — used by the
// --snaptest / --megashot harnesses so they render through the identical path.
// The interactive editor shows connector markers for the selected module only;
// `allConnectors` forces them for every module (the snap-eyeball harness wants all,
// a big-station shot wants none).
void drawScene(const Station& station, const ModuleCatalog& catalog, const ::Camera3D& camera,
               MeshCache& meshes, bool showGizmos, bool showMeshes, bool allConnectors = false,
               bool lodEnabled = true);

// Diagnostic: tally the per-module LOD/cull decisions for a camera without
// drawing, reusing the exact metrics drawScene uses. Lets a headless harness
// quantify what a large station keeps vs. culls vs. collapses-to-box per view.
struct RenderStats {
  int total{0};          // modules with a resolvable def
  int culled{0};         // rejected by the frustum cull (behind / past far / off-cone)
  int drawnDetailed{0};  // above the mesh pixel threshold (full mesh)
  int drawnBox{0};       // below it (cheap AABB box)
};
[[nodiscard]] RenderStats lodStats(const Station& station, const ModuleCatalog& catalog,
                                   const ::Camera3D& camera);

// Combined X4-space bounds of every placed module (modules with no resolvable def
// are skipped). `radius` is the bounding-sphere radius. An empty station yields a
// zeroed box/center and radius 0; callers apply their own framing default. Used by
// the F-frame key and the --megashot harness so both frame a station identically.
struct StationBounds {
  AABB box;
  Vec3 center;
  double radius{0.0};
};
[[nodiscard]] StationBounds stationBounds(const Station& station, const ModuleCatalog& catalog);

// Bounding center + sphere radius of a single AABB (the per-AABB form of
// StationBounds; `box` is returned unchanged). Shared by stationBounds and the
// F-key single-selection focus so both use one center/radius convention.
[[nodiscard]] StationBounds boundsOf(const AABB& box);

// Draw the translate/rotate gizmo (axis arrows, plane quads, rotation rings) for
// the selected module, anchored at the live drag-preview pose while dragging. The
// dragged module's own mesh moves live (see drawScene), so no AABB-box preview is
// drawn. Call inside BeginMode3D/EndMode3D.
void drawGizmo(const EditorState& state, const ::Camera3D& camera);

// Draw the 2D HUD overlay (active module, filter, counts, undo state, controls).
void drawHud(const EditorState& state, int screenWidth, int screenHeight, bool showGizmos);

// Draw a transient status line (plan save/load result) bottom-left, over the HUD.
void drawToast(const std::string& message, int screenHeight);

}  // namespace x4sb::editor

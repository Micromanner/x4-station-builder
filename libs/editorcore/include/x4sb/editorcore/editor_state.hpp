#pragma once
// EditorState (parent §5): the editor's whole interaction model as render-free
// logic — the Station being edited, its undo history, the active-module cursor,
// the current selection, and the per-frame ghost preview. The raylib shell in
// apps/editor only converts input into X4-space rays/keys and draws this state,
// so the entire interaction is unit-testable under the `core` preset.
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/data/types.hpp"
#include "x4sb/document/station.hpp"
#include "x4sb/editorcore/gizmo.hpp"
#include "x4sb/snap/snap.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace x4sb {

// The current frame's placement preview. `candidate` is empty for a root
// free-place (the first module, snapped to nothing).
struct Ghost {
  std::string defId;
  Transform worldTransform{};
  bool valid{false};  // safe to commit (no collision, has a target or is root)?
  std::optional<SnapCandidate> candidate;
};

// The connector pair about to snap, in world space, for the renderer's approach
// guide-line. Both points are at the active module's CURRENT pose, so as it snaps
// they converge to the joint. Spec §5.
struct SnapLink {
  Vec3 fromWorld;  // active/ghost/dragged module's connector
  Vec3 toWorld;    // target module's connector
};

class EditorState {
 public:
  explicit EditorState(const ModuleCatalog& catalog);

  // ── Active-module cursor ────────────────────────────────────────────────
  [[nodiscard]] const ModuleDef* activeDef() const;  // nullptr if filtered view empty
  [[nodiscard]] std::size_t activeIndex() const { return activeIndex_; }
  [[nodiscard]] std::size_t activeCount() const;  // size of the filtered view
  void cycleActive(int delta);                    // wraps; no-op if view empty
  void setFilter(std::optional<Category> cat);    // resets activeIndex to 0
  [[nodiscard]] std::optional<Category> filter() const { return filter_; }

  // ── Placement ───────────────────────────────────────────────────────────
  // Recompute the ghost preview from a mouse ray in X4 space. On a non-empty
  // station: snap to the nearest free compatible connector under the ray; if none
  // (or forceFree), free-place at a view-facing standoff (rayOrigin + rayDir *
  // placeDistance_) carrying pendingRotation_. No ghost when placement is disabled
  // (select mode) or the active view is empty.
  void updateGhost(Vec3 rayOriginX4, Vec3 rayDirX4, bool forceFree = false);
  [[nodiscard]] const std::optional<Ghost>& ghost() const { return ghost_; }
  // Commit the current ghost if valid; returns the new instance id or nullopt.
  std::optional<InstanceId> commitGhost();
  // Accumulate a +90deg rotation about a world axis into the placement ghost's
  // pending rotation (applied to free placements; reset to identity on commit).
  void rotateGhost(Vec3 worldAxis);

  // The free-place ghost floats this far in front of the camera along the cursor
  // ray — a view-facing standoff, NOT a ground-plane projection, so vertical mouse
  // motion moves the ghost up/down on screen instead of near/far. The shell captures
  // this from the orbit distance when build mode is entered and then holds it, so
  // zooming doesn't drag the ghost in/out; the default is a headless-test value.
  void setPlaceDistance(double distance) { placeDistance_ = distance; }
  // Placement (build) mode vs. select mode. When disabled there is no ghost, so a
  // left-click selects an existing module instead of placing a new one — letting
  // you pick a module you already built and keep building onto it. Toggled by the
  // shell's holster key.
  void togglePlacement() {
    placementEnabled_ = !placementEnabled_;
    if (!placementEnabled_) ghost_.reset();
  }
  [[nodiscard]] bool placementEnabled() const { return placementEnabled_; }

  // X4's editor blocks overlapping module bodies; this toggle (default off)
  // bypasses the body-overlap (AABB) check only — dock flight-corridor clearance
  // stays enforced (verified in-game: overlap-on still blocks corridor placement).
  // Export is unaffected — X4 accepts overlapping imports unconditionally
  // (known-issues §1.1 / §2.1).
  void setAllowOverlap(bool allow) { allowOverlap_ = allow; }
  [[nodiscard]] bool allowOverlap() const { return allowOverlap_; }

  // Render-only: when true the editor draws EVERY dock's flight-corridor clearance
  // volume, not just the selected/ghost module's. Has no effect on placement validity.
  void setShowAllClearance(bool show) { showAllClearance_ = show; }
  [[nodiscard]] bool showAllClearance() const { return showAllClearance_; }

  // ── History ─────────────────────────────────────────────────────────────
  void undo() {
    undo_.undo(station_);
    gridDirty_ = true;
  }
  void redo() {
    undo_.redo(station_);
    gridDirty_ = true;
  }
  [[nodiscard]] bool canUndo() const { return undo_.canUndo(); }
  [[nodiscard]] bool canRedo() const { return undo_.canRedo(); }
  // Replace the document on plan load: a fresh document, so undo/redo,
  // selection, and ghost reset.
  void loadStation(Station station);

  // ── Selection / deletion ────────────────────────────────────────────────
  // Pick the nearest module along the X4-space ray; sets (or clears on a miss)
  // the selection. Returns the selected id, if any.
  std::optional<InstanceId> selectByRay(Vec3 rayOriginX4, Vec3 rayDirX4);
  [[nodiscard]] std::optional<InstanceId> selected() const { return selected_; }
  void clearSelection() { selected_.reset(); }
  bool deleteSelected();

  // ── Gizmo drag (move) ─────────────────────────────────────────────────────
  // Begin a translate-gizmo drag if the X4-space ray picks a handle on the
  // selected module. `gizmoScale` is the renderer's on-screen handle length
  // (world units). Clears any ghost. Returns false if nothing was grabbed.
  bool beginGizmoDrag(Vec3 rayOriginX4, Vec3 rayDirX4, double gizmoScale);
  // Update the in-progress drag; recomputes the preview and engages snap-on-move
  // (magnetic, within dragSnapRadius_) unless forceFree.
  void updateGizmoDrag(Vec3 rayOriginX4, Vec3 rayDirX4, bool forceFree = false);
  // Commit the drag: SnapMoveCommand if snapped, else MoveModuleCommand. Returns
  // false (no command) for a zero-distance click.
  bool endGizmoDrag();
  void cancelGizmoDrag() { drag_.reset(); }
  [[nodiscard]] bool dragging() const { return drag_.has_value(); }
  [[nodiscard]] std::optional<Transform> dragPreview() const;

  // Rotate the selected placed module +90deg about a world axis (detaches; one
  // undoable MoveModuleCommand). No-op without a selection.
  bool rotateSelected(Vec3 worldAxis);

  // Recompute which gizmo handle the cursor ray hovers (for the renderer's
  // highlight). Cleared when dragging, unselected, or the ray misses every handle.
  void updateGizmoHover(Vec3 rayOriginX4, Vec3 rayDirX4, double gizmoScale);
  // The handle to highlight: the active drag handle while dragging, else the hover.
  [[nodiscard]] std::optional<GizmoHandle> highlightHandle() const;

  // Translate vs Rotate gizmo handle set. Resets to Translate when a module is
  // selected (most-common action); the shell's T/Y keys flip it.
  [[nodiscard]] GizmoMode gizmoMode() const { return gizmoMode_; }
  void setGizmoMode(GizmoMode m) { gizmoMode_ = m; }

  // ── Read access for the renderer ────────────────────────────────────────
  [[nodiscard]] const Station& station() const { return station_; }
  // The catalog this state was built with (for the renderer's def lookups).
  [[nodiscard]] const ModuleCatalog& catalog() const { return catalog_; }
  // Resolve a module definition from the catalog this state was built with.
  [[nodiscard]] const ModuleDef* defFor(const std::string& defId) const;
  // Broad-phase connector index for snap search and connector rendering. Built
  // lazily and rebuilt only after a mutation (placement/move/delete/undo/redo/
  // load) — never per frame. Logically const: the cache does not change the
  // observable document, so it is exposed to the const renderer path.
  [[nodiscard]] const ConnectorGrid& connectorGrid() const;

  // EVERY compatible free connector pair within the approach radius of the active
  // ghost (placement) or dragged module, at its current pose — for the renderer's
  // snap guide-lines. Its own (wider) search, independent of the committed snap, so it
  // shows ALL possible snaps, not just the nearest. Empty when nothing is in range or
  // there is no ghost/drag. Spec §5.2.
  [[nodiscard]] std::vector<SnapLink> activeSnapLinks() const;

 private:
  [[nodiscard]] std::vector<std::string> filteredOrder() const;
  // Single funnel for committing commands so every mutation dirties the grid.
  void execute(std::unique_ptr<Command> cmd);
  // Placement validity for a candidate body at `xf`: body overlap is bypassable
  // via allowOverlap_, but dock/cradle clearance corridors always block (verified
  // in-game — "Allow Module Overlap" relaxes body AABBs only). `ignoreA`/`ignoreB`
  // skip the moved module and its snap partner; either may be 0.
  [[nodiscard]] bool placementClear(const ModuleDef& def, const Transform& xf, InstanceId ignoreA,
                                    InstanceId ignoreB) const;

  const ModuleCatalog& catalog_;
  Station station_;
  UndoStack undo_;
  std::vector<std::string> order_;  // player-buildable module ids, sorted (stable cursor)
  std::optional<Category> filter_;
  std::size_t activeIndex_{0};
  std::optional<InstanceId> selected_;
  std::optional<Ghost> ghost_;
  Quat pendingRotation_{};   // orients the free-place ghost; reset on commit
  bool placementEnabled_{true};  // false = select mode (no ghost; clicks select)
  bool allowOverlap_{false};  // false = block overlap (mirrors X4's editor default)
  bool showAllClearance_{false};  // render-only: draw every dock's corridor when true

  mutable std::optional<ConnectorGrid> connectorGrid_;  // built lazily by connectorGrid()
  mutable bool gridDirty_{true};                        // true => rebuild on next access

  // In-progress gizmo drag (none when not dragging).
  struct GizmoDrag {
    InstanceId id{0};
    GizmoHandle handle{};
    Vec3 startRayOrigin{};
    Vec3 startRayDir{};
    Transform startTransform{};
    Transform preview{};
    std::optional<SnapCandidate> snap;
  };
  std::optional<GizmoDrag> drag_;
  std::optional<GizmoHandle> hoveredHandle_;  // gizmo handle under the cursor, if any
  GizmoMode gizmoMode_{GizmoMode::Translate};
  // Snap-on-move is magnetic but only near the joint, so it uses a tighter radius
  // than the cursor-proxy placement snapRadius_ (which is large because the ghost
  // cursor is a far box-surface hit, spec §6). Tunable.
  double dragSnapRadius_{300.0};

  // Approach radius for the guide-line: wider than the snap radii so the line shows
  // DURING the approach, before the instant magnetic snap coincides the connectors
  // (like the proximity glow). Tunable.
  double lineRadius_{2000.0};

  // Tunables (parent §6); public access added later if the UI exposes them.
  // snapRadius is in X4 world units: modules span up to ~1500 and connectors sit
  // at the module extents, so the nearest free connector to a ray's box-surface
  // hit point can be ~1000+ away — 50 was far too small to ever snap.
  double snapRadius_{1000.0};
  // View-facing standoff for free placement (see setPlaceDistance). The shell sets
  // this to a stable baseline on build-mode entry; 10.0 is the headless default.
  double placeDistance_{10.0};
};

}  // namespace x4sb

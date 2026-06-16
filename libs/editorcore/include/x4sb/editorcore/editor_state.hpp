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
#include "x4sb/snap/snap.hpp"

#include <cstddef>
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
  // Recompute the ghost preview from a mouse ray in X4 space. Empty station ->
  // a root ghost at the ray's intersection with the ground plane (y = groundY).
  // (Snap placement on a non-empty station lands in Task 4.)
  void updateGhost(Vec3 rayOriginX4, Vec3 rayDirX4);
  [[nodiscard]] const std::optional<Ghost>& ghost() const { return ghost_; }
  // Commit the current ghost if valid; returns the new instance id or nullopt.
  std::optional<InstanceId> commitGhost();

  // ── History ─────────────────────────────────────────────────────────────
  void undo() { undo_.undo(station_); }
  void redo() { undo_.redo(station_); }
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

  // ── Read access for the renderer ────────────────────────────────────────
  [[nodiscard]] const Station& station() const { return station_; }
  // The catalog this state was built with (for the renderer's def lookups).
  [[nodiscard]] const ModuleCatalog& catalog() const { return catalog_; }
  // Resolve a module definition from the catalog this state was built with.
  [[nodiscard]] const ModuleDef* defFor(const std::string& defId) const;

 private:
  [[nodiscard]] std::vector<std::string> filteredOrder() const;

  const ModuleCatalog& catalog_;
  Station station_;
  UndoStack undo_;
  std::vector<std::string> order_;  // all module ids, sorted (stable cursor)
  std::optional<Category> filter_;
  std::size_t activeIndex_{0};
  std::optional<InstanceId> selected_;
  std::optional<Ghost> ghost_;

  // Tunables (parent §6); public access added later if the UI exposes them.
  // snapRadius is in X4 world units: modules span up to ~1500 and connectors sit
  // at the module extents, so the nearest free connector to a ray's box-surface
  // hit point can be ~1000+ away — 50 was far too small to ever snap.
  double snapRadius_{2000.0};
  double groundY_{0.0};
};

}  // namespace x4sb

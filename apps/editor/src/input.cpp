#include "input.hpp"

#include "raylib_convert.hpp"  // toRl

#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/snap/pick.hpp"

#include "raymath.h"  // Vector3Distance

#include <array>
#include <cstddef>
#include <limits>
#include <optional>

namespace x4sb::editor {

void mouseRayX4(const ::Camera3D& camera, Vec3& originOut, Vec3& dirOut) {
  // raylib 5.5: GetScreenToWorldRay replaces the deprecated GetMouseRay.
  const ::Ray r = GetScreenToWorldRay(GetMousePosition(), camera);
  originOut = flipZ(Vec3{r.position.x, r.position.y, r.position.z});
  dirOut = flipZ(Vec3{r.direction.x, r.direction.y, r.direction.z});
}

void handleKeys(EditorState& state) {
  if (IsKeyPressed(KEY_LEFT_BRACKET)) state.cycleActive(-1);
  if (IsKeyPressed(KEY_RIGHT_BRACKET)) state.cycleActive(1);

  // Category filter on number keys 1-N (KEY_ONE.. are contiguous), one per
  // Category in declaration order. Bound derived from the array so adding a
  // Category can't desync a hard-coded count.
  const std::array<Category, 7> cats{Category::Production, Category::Storage, Category::Habitat,
                                     Category::Dock,       Category::Defense, Category::Connector,
                                     Category::Other};
  for (std::size_t i = 0; i < cats.size(); ++i) {
    if (IsKeyPressed(KEY_ONE + static_cast<int>(i))) {
      const std::optional<Category> prev = state.filter();
      state.setFilter(cats[i]);
      if (state.activeCount() == 0) state.setFilter(prev);  // empty group: keep current
    }
  }
  if (IsKeyPressed(KEY_ZERO)) state.setFilter(std::nullopt);

  if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_X)) state.deleteSelected();

  const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
  if (ctrl && IsKeyPressed(KEY_Z)) state.undo();
  if (ctrl && IsKeyPressed(KEY_Y)) state.redo();

  // Rotate 90deg: R = yaw (Y), Shift+R = pitch (X), Ctrl+R = roll (Z). Axes are in
  // X4 space (the renderer applies the (1,1,-1) flip). Selection rotates in place;
  // otherwise the placement ghost's pending rotation accumulates.
  if (IsKeyPressed(KEY_R)) {
    Vec3 axis{0, 1, 0};
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
      axis = {1, 0, 0};
    } else if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
      axis = {0, 0, 1};
    }
    if (state.selected()) {
      state.rotateSelected(axis);
    } else {
      state.rotateGhost(axis);
    }
  }

  // Q holsters the active module (toggle build <-> select mode). In select mode
  // there is no ghost, so a left-click selects a module you already built — then
  // press Q again to keep building onto it.
  if (IsKeyPressed(KEY_Q)) state.togglePlacement();

  // Gizmo mode: T = Translate (arrows/planes), Y = Rotate (rings). Guarded against
  // Ctrl so Ctrl+Y (redo) does not also switch mode.
  if (!ctrl && IsKeyPressed(KEY_T)) state.setGizmoMode(GizmoMode::Translate);
  if (!ctrl && IsKeyPressed(KEY_Y)) state.setGizmoMode(GizmoMode::Rotate);
}

double gizmoScaleFor(const ::Camera3D& camera, const EditorState& state) {
  constexpr double kDefault = 50.0;
  if (!state.selected()) return kDefault;
  const PlacedModule* m = state.station().find(*state.selected());
  if (m == nullptr) return kDefault;
  // Camera-space depth to the module gives a constant on-screen size; the module's OWN
  // Euclidean eye distance is the collapse floor (NOT the orbit distance, which would make
  // the size track the zoom pivot). The math lives in editorcore::gizmoScale, shared with
  // the hit-test path.
  const ::Vector3 mp = toRl(flipZ(m->worldTransform.position));  // display space
  const ::Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
  const double depth =
      static_cast<double>(Vector3DotProduct(Vector3Subtract(mp, camera.position), fwd));
  const double dist = static_cast<double>(Vector3Distance(camera.position, mp));
  // Cap the handle to the module's own size so it isn't swallowed by a constant-pixel handle
  // when the part is small/far (zoomed out) — past the cap it shrinks with the part. The cap
  // is module-relative (not a fixed world constant) because X4 parts span tiny connectors to
  // km-scale hulls. kMaxModuleRadii is a pure visual tuning knob (how many bounding-sphere
  // radii the arms may reach before they stop growing).
  constexpr double kMaxModuleRadii = 1.5;
  const ModuleDef* def = state.catalog().find(m->defId);
  const double maxScale = def != nullptr
                              ? length(def->aabb.max - def->aabb.min) * 0.5 * kMaxModuleRadii
                              : std::numeric_limits<double>::infinity();
  return gizmoScale(depth, dist, maxScale);
}

void handleMouse(EditorState& state, const ::Camera3D& camera) {
  const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
  const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  Vec3 ro{};
  Vec3 rd{};
  mouseRayX4(camera, ro, rd);

  // Free placement floats the ghost at the camera's orbit distance (the look-at
  // depth) along the cursor ray — a view-facing standoff, so mouse up/down moves
  // the ghost up/down on screen instead of near/far across a ground plane.
  const float orbit = Vector3Distance(camera.position, camera.target);
  state.setPlaceDistance(orbit > 1.0F ? static_cast<double>(orbit) : 1.0);

  // Ghost preview only when not dragging (a drag owns the selection's pose).
  if (!state.dragging()) state.updateGhost(ro, rd, alt || shift);

  // One scale read per frame (camera + selection are fixed here), shared by the
  // hover highlight and the grab hit-test — each call does an O(n) Station::find.
  const double scale = gizmoScaleFor(camera, state);
  state.updateGizmoHover(ro, rd, scale);

  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    // A gizmo grab takes priority over commit/select.
    if (!state.beginGizmoDrag(ro, rd, scale)) {
      const bool placed = state.ghost().has_value() && state.ghost()->valid &&
                          state.commitGhost().has_value();
      if (!placed) state.selectByRay(ro, rd);
    }
  }
  if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && state.dragging()) {
    state.updateGizmoDrag(ro, rd, alt || shift);
  }
  if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && state.dragging()) {
    state.endGizmoDrag();
  }
}

}  // namespace x4sb::editor

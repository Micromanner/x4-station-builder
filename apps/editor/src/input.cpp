#include "input.hpp"

#include "x4sb/editorcore/display_flip.hpp"

#include <array>
#include <cstddef>
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
}

}  // namespace x4sb::editor

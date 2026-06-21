#pragma once
// Left build palette: category filter chips, scrollable module list with per-row
// quantity steppers, Auto Build toggle, and a Total/Build footer. Declared in Clay
// each frame from a read-only EditorState; emits a PaletteAction intent that the
// caller (main.cpp) dispatches (SetFilter, SetActive, ToggleAutoBuild, AdjustCart,
// Build). UI reads state, never mutates it.
#include "x4sb/editorcore/editor_state.hpp"

#include <cstddef>
#include <string>

namespace x4sb::editor {

enum class PaletteActionKind {
  None,
  SetFilter,
  ClearFilter,
  SetActive,
  ToggleAutoBuild,
  AdjustCart,
  Build
};

struct PaletteAction {
  PaletteActionKind kind{PaletteActionKind::None};
  Category category{Category::Production};  // valid when kind == SetFilter
  std::size_t index{0};                     // valid when kind == SetActive (filteredView index)
  std::string id;  // valid when kind == AdjustCart (cart key — the module id)
  int delta{0};    // valid when kind == AdjustCart (+/-1 or +/-10)
};

[[nodiscard]] float paletteWidth();

// Declare the palette panel (caller is between Clay_BeginLayout/EndLayout). Returns
// this frame's clicked intent, if any.
[[nodiscard]] PaletteAction palette(const EditorState& state);

}  // namespace x4sb::editor

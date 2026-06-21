#pragma once
// The left build palette: clickable category filter chips + a scrollable, click-to-
// select module list + a one-line cart summary, declared in Clay each frame from a
// read-only EditorState. Emits a PaletteAction intent; the caller (main.cpp)
// dispatches it into setFilter/setActiveIndex. UI reads state, never mutates it.
#include "x4sb/editorcore/editor_state.hpp"

#include <cstddef>

namespace x4sb::editor {

enum class PaletteActionKind { None, SetFilter, ClearFilter, SetActive };

struct PaletteAction {
  PaletteActionKind kind{PaletteActionKind::None};
  Category category{Category::Production};  // valid when kind == SetFilter
  std::size_t index{0};                     // valid when kind == SetActive (filteredView index)
};

[[nodiscard]] float paletteWidth();

// Declare the palette panel (caller is between Clay_BeginLayout/EndLayout). Returns
// this frame's clicked intent, if any.
[[nodiscard]] PaletteAction palette(const EditorState& state);

}  // namespace x4sb::editor

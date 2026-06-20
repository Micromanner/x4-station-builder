#pragma once
// The floating top bar: file/history buttons + plot/placed/FPS readouts, declared
// in Clay each frame from read-only EditorState. Emits a TopBarAction intent; the
// caller (main.cpp) dispatches it. UI reads state, never mutates it.
#include "x4sb/editorcore/editor_state.hpp"

namespace x4sb::editor {

enum class TopBarAction { None, Open, Save, Undo, Redo };

[[nodiscard]] float topBarHeight();

// Declare the bar (caller has already called Clay_BeginLayout + set pointer state).
// Returns the clicked action, if any, for this frame.
[[nodiscard]] TopBarAction topBar(const EditorState& state, double fps);

}  // namespace x4sb::editor

#pragma once
// The right inspector panel: context-driven module detail, declared in Clay each
// frame from a read-only EditorState. Shows the SELECTED placed module if one is
// selected, else the ACTIVE build module. Pure display — no intent. Fields come from
// existing catalog data only (no production/cost/workforce — not in the pipeline).
#include "x4sb/editorcore/editor_state.hpp"

namespace x4sb::editor {

[[nodiscard]] float inspectorWidth();

// Declare the inspector panel (caller is between Clay_BeginLayout/EndLayout).
void inspector(const EditorState& state);

}  // namespace x4sb::editor

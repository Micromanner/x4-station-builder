#pragma once
// Plan Save/Load glue for the editor shell (build-order step 4). Lives here rather
// than in editorcore because it touches the clipboard and the filesystem.
#include "x4sb/editorcore/editor_state.hpp"

#include <optional>
#include <string>

namespace x4sb::editor {

// What one frame's plan-IO key handling did.
struct PlanIoOutcome {
  std::optional<std::string> message;  // one-line HUD status when an action ran
  bool reloaded{false};  // Ctrl+O loaded a new station -> caller should pre-warm its meshes
};

// Handle Ctrl+S (export station -> <profile>/constructionplan/x4sb_plan.xml +
// clipboard) and Ctrl+O (load newest *.xml there -> EditorState) for this frame.
[[nodiscard]] PlanIoOutcome handlePlanIoKeys(EditorState& state);

}  // namespace x4sb::editor

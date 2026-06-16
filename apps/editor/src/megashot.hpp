#pragma once
// Diagnostic harness (hidden `--megashot <planPath> <outPrefix>` flag). Loads a
// real construction plan, prints whole-station bounds + per-view LOD/cull tallies,
// and screenshots framed/shell/inside cameras through the REAL render path. Used to
// gather evidence on the big-station disappearing-geometry + perf bugs without
// driving the interactive editor. apps/editor only (links GL).
#include <string>

namespace x4sb::editor {

[[nodiscard]] int runMegaShot(const std::string& planPath, const std::string& outPrefix);

}  // namespace x4sb::editor

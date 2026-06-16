#pragma once
// Headless RenderDoc frame-capture harness (hidden `--rdc <planPath>` flag). Loads
// a plan, warms up (mesh upload), then programmatically brackets ONE frame with the
// RenderDoc in-app API and exits — so a capture is deterministic and needs no key
// press. When not launched under RenderDoc it just renders the frame and says so.
// Inspect the resulting .rdc in qrenderdoc.exe. apps/editor only (links GL).
#include <string>

namespace x4sb::editor {

[[nodiscard]] int runRdcCapture(const std::string& planPath);

}  // namespace x4sb::editor

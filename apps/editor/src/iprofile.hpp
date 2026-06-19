#pragma once
// Hidden interactive-profile harness: like --profile, but drives a live ghost at the
// station centre each frame and renders through the EditorState drawScene path (the
// one the editor actually uses), so the connector markers / snap-lines / clearance /
// gizmo draws that only fire during placement get measured. --profile renders the bare
// Station overload and misses all of that.
#include <string>

namespace x4sb::editor {
int runInteractiveProfile(const std::string& planPath, int frames);
}  // namespace x4sb::editor

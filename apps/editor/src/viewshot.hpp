#pragma once
// Diagnostic harness (hidden `--viewshot <planPath> <viewJson> <outPrefix>` flag).
// Re-renders the exact view the user was looking at when they pressed F12 in the
// editor (livedump_view.json + livedump_plan.xml: camera, window size, sky cluster,
// exposure) through the REAL frame pipeline, once per hull-lighting term, and dumps
// the HDR resolved target of each as half-float .raw so a visual complaint can be
// attributed to a term numerically instead of by eye. apps/editor only (links GL).
#include <string>

namespace x4sb::editor {

struct NativeAssets;

[[nodiscard]] int runViewShot(const NativeAssets& native, const std::string& planPath,
                              const std::string& viewJsonPath, const std::string& outPrefix);

}  // namespace x4sb::editor

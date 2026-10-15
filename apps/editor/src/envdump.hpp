#pragma once
#include <string>

namespace x4sb::editor {

struct NativeAssets;
// Hidden harness: bakes the env cubemap, writes the 6 faces as
// <prefix>_face{0..5}.png and a mirror-probe render as <prefix>_probe.png.
// Returns a process exit code.
int runEnvDump(const NativeAssets& native, const std::string& outPrefix);
}  // namespace x4sb::editor

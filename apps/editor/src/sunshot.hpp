#pragma once
#include <string>

namespace x4sb::editor {

struct NativeAssets;
// Hidden harness: renders the named baked cluster's sky through the real
// HDR+bloom frame pipeline and writes <prefix>_sun.png. The measurable A/B
// target for sun lens-effect calibration against captured game frames
// (tools/ab_measure.py). Defaults = the live editor's units, sun on-axis.
struct SunShotOptions {
  float sceneExposure{1.0f};
  int width{1280};
  int height{720};
  float fovy{60.0f};
  // Where the sun lands on screen (NDC, +y up); 0,0 = centred.
  float sunNdcX{0.0f};
  float sunNdcY{0.0f};
  bool drawGlow{true};    // the HDR glow=1 half
  bool drawFlares{true};  // the post-tonemap half
};
int runSunShot(const NativeAssets& native, const std::string& clusterDir,
               const std::string& outPrefix, const SunShotOptions& options = {});
}  // namespace x4sb::editor

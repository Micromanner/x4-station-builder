#pragma once
// Frame tuning panel (F10): a live +/- stepper for the pre-tonemap scene
// exposure (1.0 = X4's units through the captured LUT), so reference-matching
// iterates without rebuilds. Render-only; emits step deltas, main.cpp owns the
// value and applies it to the FramePipeline.

namespace x4sb::editor {

struct SunTuneState {
  bool open = false;
  float sceneExposure = 0.0f;
};

struct SunTuneResult {
  int sceneExpDelta = 0;  // steps of 0.05
};

// Declare the floating panel (call inside the Clay chrome root when open).
void sunTuningPanel(const SunTuneState& s, SunTuneResult& out);

}  // namespace x4sb::editor

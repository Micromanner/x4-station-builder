#pragma once
// Hidden perf harness for known-issues 1.2b: builds a synthetic big station and
// times the per-frame connector pass three ways — no connectors (baseline), ALL
// connectors (the pre-fix O(modules x connectors) hot path, via allConnectors),
// and grid-culled with a selection active (the fix). Reports mean/p95 ms + FPS so
// the render-side snap-perf win is measurable without the real game's mega plan.
#include <string>

namespace x4sb::editor {

// modules = synthetic station size; frames = timed frames per configuration.
// spacingOverride > 0 forces the grid spacing (small = dense station, which is
// what exposes the big-selection reach blow-up); 0 = auto from the module AABB.
int runConnBench(int modules, int frames, double spacingOverride = 0.0);

}  // namespace x4sb::editor

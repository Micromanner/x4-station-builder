#pragma once
// Headless profiling/benchmark harness (hidden `--profile <planPath> <frames>`
// flag). Loads a real construction plan and renders an orbiting camera for N
// frames through the REAL render path, then prints frame-time percentiles
// (p50/p95/p99/max) — a comparison-free CPU-cost signal that needs no GUI.
//
// Built with the full-tracy preset it ALSO emits a Tracy zone per frame, so the
// `capture` CLI records a clean, reproducible frame stream to inspect per-zone CPU
// time. Decoupling the workload from interactive input is what makes a capture
// deterministic and agent-drivable. apps/editor only (links GL).
#include <string>

namespace x4sb::editor {

[[nodiscard]] int runProfile(const std::string& planPath, int frames);

}  // namespace x4sb::editor

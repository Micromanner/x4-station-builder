#pragma once
// Hidden perf/correctness harness for the loading-screen pre-upload (known-issues
// 1.3). Builds a synthetic station spanning many distinct catalog macros (so it has
// a realistic count of unique meshes), runs loadStationMeshes, and reports the load
// time plus how many meshes ended up resident — proving the whole plan is uploaded
// up front (no streaming / no pop-in).
namespace x4sb::editor {

int runLoadBench(int modules);

}  // namespace x4sb::editor

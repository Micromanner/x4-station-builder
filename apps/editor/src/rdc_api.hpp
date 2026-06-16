#pragma once
// RenderDoc in-application API shim. Isolated from raylib (its impl includes
// <windows.h>, which collides with raylib.h on Rectangle/CloseWindow/etc.), so
// callers get programmatic frame capture without dragging Win32 into the renderer.
//
// renderdoc.dll is present in the process ONLY when it was launched via
// `renderdoccmd capture` / `inject` (see tools/rdc-capture.ps1). In a normal run
// available() is false and the capture calls are no-ops — this header costs nothing
// and needs no build flag.
namespace x4sb::editor::rdc {

// True when launched under RenderDoc and the in-app API loaded successfully.
[[nodiscard]] bool available();

// Bracket exactly the frame(s) to capture. nullptr device/window = the active GL
// context + window (correct for the single-window editor). No-ops if unavailable.
void startFrameCapture();
void endFrameCapture();

}  // namespace x4sb::editor::rdc

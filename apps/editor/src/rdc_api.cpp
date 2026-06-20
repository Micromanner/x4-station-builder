#include "rdc_api.hpp"

#include "renderdoc_app.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace x4sb::editor::rdc {
namespace {

// renderdoc.dll is injected only when the process is launched via
// `renderdoccmd capture`/`inject`; a normal run finds no module and returns null,
// making every capture call a no-op. Requesting the 1.4.0 API (StartFrameCapture
// has existed since 1.0.0) succeeds against any newer RenderDoc runtime.
RENDERDOC_API_1_4_0* loadApi() {
  const HMODULE mod = GetModuleHandleA("renderdoc.dll");
  if (mod == nullptr) return nullptr;

  // GetProcAddress yields FARPROC; the reinterpret_cast to the typed entry point is
  // the documented RenderDoc idiom. -Wcast-function-type (via -Wextra) flags the
  // signature change — suppress just at this one well-understood cast.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
  const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  if (getApi == nullptr) return nullptr;

  RENDERDOC_API_1_4_0* api = nullptr;
  if (getApi(eRENDERDOC_API_Version_1_4_0, reinterpret_cast<void**>(&api)) != 1) return nullptr;
  return api;
}

// Loaded once on first use; null when not running under RenderDoc.
RENDERDOC_API_1_4_0* api() {
  static RENDERDOC_API_1_4_0* const cached = loadApi();
  return cached;
}

}  // namespace

bool available() { return api() != nullptr; }

void startFrameCapture() {
  if (RENDERDOC_API_1_4_0* a = api()) a->StartFrameCapture(nullptr, nullptr);
}

void endFrameCapture() {
  if (RENDERDOC_API_1_4_0* a = api()) a->EndFrameCapture(nullptr, nullptr);
}

}  // namespace x4sb::editor::rdc

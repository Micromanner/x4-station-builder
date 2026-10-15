#pragma once
// OpenGL pieces raylib's bundled glad lacks (design 2026-10-14 §2): the
// ARB_bindless_texture entry points, and detection of the extensions the
// station renderer requires.
#include "external/glad.h"

#include <string>
#include <vector>

namespace x4sb::editor {

using GetTextureHandleProc = GLuint64(GLAD_API_PTR*)(GLuint texture);
using MakeTextureHandleResidentProc = void(GLAD_API_PTR*)(GLuint64 handle);

struct GlExtensions {
  GetTextureHandleProc getTextureHandle{};
  MakeTextureHandleResidentProc makeTextureHandleResident{};
  MakeTextureHandleResidentProc makeTextureHandleNonResident{};
  std::vector<std::string> missing;  // required extensions the driver lacks
};

// Needs a current GL context (after InitWindow).
[[nodiscard]] GlExtensions loadGlExtensions();
// Interactive startup gate: when anything is missing, shows why in the window
// until it is closed (a Release build has no console) and returns false.
[[nodiscard]] bool requireGlExtensions();

}  // namespace x4sb::editor

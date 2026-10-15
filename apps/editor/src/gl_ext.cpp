#include "gl_ext.hpp"

#include "raylib.h"

#define GLFW_INCLUDE_NONE
#include "external/glfw/include/GLFW/glfw3.h"

#include <array>
#include <string_view>
#include <unordered_set>

namespace x4sb::editor {
namespace {

template <typename Proc>
Proc load(const char* name) {
  return reinterpret_cast<Proc>(glfwGetProcAddress(name));
}

}  // namespace

GlExtensions loadGlExtensions() {
  std::unordered_set<std::string_view> present;
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; ++i)
    if (const auto* name = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)))
      present.insert(reinterpret_cast<const char*>(name));

  GlExtensions ext;
  // Material texture handles live in an SSBO indexed per sub-draw (gl_DrawIDARB),
  // which keeps every handle dynamically uniform, so no non-uniform-index extension is
  // needed (Intel's Windows compiler has none). The fragment stages broadcast that
  // index with readFirstInvocationARB so the compiler sees it as uniform too.
  constexpr std::array<std::string_view, 3> kRequired{
      "GL_ARB_bindless_texture", "GL_ARB_shader_draw_parameters", "GL_ARB_shader_ballot"};
  for (const std::string_view name : kRequired)
    if (present.count(name) == 0) ext.missing.emplace_back(name);

  ext.getTextureHandle = load<GetTextureHandleProc>("glGetTextureHandleARB");
  ext.makeTextureHandleResident =
      load<MakeTextureHandleResidentProc>("glMakeTextureHandleResidentARB");
  ext.makeTextureHandleNonResident =
      load<MakeTextureHandleResidentProc>("glMakeTextureHandleNonResidentARB");
  if (ext.missing.empty() &&
      (ext.getTextureHandle == nullptr || ext.makeTextureHandleResident == nullptr ||
       ext.makeTextureHandleNonResident == nullptr))
    ext.missing.emplace_back("GL_ARB_bindless_texture entry points");
  return ext;
}

bool requireGlExtensions() {
  const GlExtensions ext = loadGlExtensions();
  if (ext.missing.empty()) return true;
  std::string list;
  for (const std::string& name : ext.missing) {
    TraceLog(LOG_ERROR, "GL: missing required %s", name.c_str());
    list += "  " + name + "\n";
  }
  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(BLACK);
    DrawText("This GPU driver lacks OpenGL features the station renderer needs:", 40, 40, 20,
             RAYWHITE);
    DrawText(list.c_str(), 40, 80, 20, ORANGE);
    DrawText("Update the graphics driver (Intel needs Skylake / Gen9 or newer).", 40, 160, 20,
             RAYWHITE);
    EndDrawing();
  }
  return false;
}

}  // namespace x4sb::editor

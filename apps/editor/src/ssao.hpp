#pragma once
// X4's screen-space ambient occlusion (p1/high/ssao_v2.frag.glsl + the old
// depth-aware blur of blur_common.glsl), run on the station's depth prepass.
// Parameters are the ones station.rdc bound, not the xu_ssao.xml defaults.
// Needs a live GL context: construct after InitWindow, destroy before CloseWindow.
#include "raylib.h"
#include "scene_depth.hpp"

#include <array>

namespace x4sb::editor {

class Ssao {
 public:
  Ssao();
  ~Ssao();
  Ssao(const Ssao&) = delete;
  Ssao& operator=(const Ssao&) = delete;
  Ssao(Ssao&&) = delete;
  Ssao& operator=(Ssao&&) = delete;

  // Occlusion for the depth in the bound draw framebuffer, seen through the
  // current rlgl projection and clip planes. False (texture stale) when there is
  // no scene target, the projection is not perspective or a shader failed.
  [[nodiscard]] bool compute();
  // R8, 1 = unoccluded; sample it by screen uv.
  [[nodiscard]] unsigned int texture() const { return tex_[0]; }
  // The scene target it was computed for, in pixels.
  [[nodiscard]] const std::array<float, 2>& screenSize() const { return depth_.size(); }

 private:
  void resize(std::array<int, 2> dims);
  void release();

  SceneDepthCopy depth_;
  ::Shader aoShader_{};
  ::Shader blurShader_{};
  unsigned int vao_{0};
  std::array<unsigned int, 2> tex_{};
  std::array<unsigned int, 2> fbo_{};
  std::array<int, 2> dims_{};
};

}  // namespace x4sb::editor

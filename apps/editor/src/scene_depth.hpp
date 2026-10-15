#pragma once
// A sampleable copy of the scene depth, taken between the opaque and transparent
// passes: X4's soft-particle fx (the dock forcefields) light up where they pass
// close in front of geometry. The scene target's depth is multisampled, and a
// transparent draw may not sample the depth attachment it tests against, so the
// depth is blitted into a texture of its own.
// Needs a live GL context: construct after InitWindow, destroy before CloseWindow.
#include <array>

namespace x4sb::editor {

class SceneDepthCopy {
 public:
  SceneDepthCopy() = default;
  ~SceneDepthCopy();
  SceneDepthCopy(const SceneDepthCopy&) = delete;
  SceneDepthCopy& operator=(const SceneDepthCopy&) = delete;
  SceneDepthCopy(SceneDepthCopy&&) = delete;
  SceneDepthCopy& operator=(SceneDepthCopy&&) = delete;

  // Copy the bound draw framebuffer's depth. False (and no copy) when drawing
  // straight to the backbuffer.
  [[nodiscard]] bool capture();
  [[nodiscard]] unsigned int texture() const { return texture_; }
  [[nodiscard]] const std::array<float, 2>& size() const { return size_; }

 private:
  void release();

  unsigned int texture_{0};
  unsigned int fbo_{0};
  std::array<int, 2> dims_{};
  int internalFormat_{0};
  std::array<float, 2> size_{1.0F, 1.0F};
};

}  // namespace x4sb::editor

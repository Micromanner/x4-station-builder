#pragma once
// Environment cubemap baked from the rendered sky domes: X4 lights surfaces
// with a prefiltered env map (lighting_common.glsl) — this is our runtime
// equivalent. Mips come from glGenerateMipmap (box filter), indexed by
// sqrt(roughness) exactly like X4 indexes its GGX-filtered chain; upgrade
// path (GGX prefilter) swaps mip CONTENTS, not the sampling code.

namespace x4sb::editor {

class Sky;

// 1x1 black cubemap so kFlatFsBody's samplerCube is ALWAYS defined — harness
// paths without a Sky (snaptest, gizmoshot) sample black env, never garbage.
[[nodiscard]] unsigned int makeFallbackCubemap();

class EnvCubemap {
 public:
  static constexpr int kFaceSize = 128;

  // Bakes immediately; GL context live by contract (after InitWindow).
  explicit EnvCubemap(Sky& sky);
  ~EnvCubemap();
  EnvCubemap(const EnvCubemap&) = delete;
  EnvCubemap& operator=(const EnvCubemap&) = delete;
  EnvCubemap(EnvCubemap&&) = delete;
  EnvCubemap& operator=(EnvCubemap&&) = delete;

  // Re-render the faces (after Sky::reload). Reuses the same texture id.
  // Uses BeginTextureMode/EndTextureMode internally; EndTextureMode restores
  // the DEFAULT framebuffer + screen viewport, so this must be called OUTSIDE
  // FramePipeline begin()/end() — a mid-frame call would redirect the rest of
  // that frame's draws to the backbuffer instead of the HDR target.
  void rebake(Sky& sky);

  [[nodiscard]] unsigned int textureId() const { return id_; }
  [[nodiscard]] float maxLod() const { return maxLod_; }

 private:
  unsigned int id_ = 0;
  float maxLod_ = 0.0f;
};

}  // namespace x4sb::editor

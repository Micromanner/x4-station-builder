#pragma once
// Frame pipeline = X4's post chain as captured from the game (RenderDoc
// sun.rdc / station.rdc, 2026-09-19, docs/2026-09-19-x4-rendering-pipeline-
// reverse-engineering.md): the 3D scene renders into a linear RGBA16F target
// with 4x MSAA (box-resolved, so bloom and flare energy is unchanged), then in
// X4's order: kinobloom (full-res median prefilter, threshold 7 knee
// 3.5 -> Karis-weighted first downsample, 8 box levels -> tent upsample
// accumulate -> tent add x0.5), the tonemap (a 32^3 log10-domain LUT from our
// fitted model of X4's look + the colour-grade CLUT, a generated neutral ramp),
// radial chromatic aberration, then the post-tonemap overlay (the sun's
// lens-flare ghosts), and finally the sRGB encode to the backbuffer. 2D UI
// drawn after end() is untouched.
#include "raylib.h"

#include <array>
#include <functional>
#include <string>

namespace x4sb::editor {

// RGBA16F offscreen target (mirrors raylib's LoadRenderTexture bookkeeping so
// BeginTextureMode/EndTextureMode work unmodified). Shared by the frame
// pipeline and the env-cubemap bake.
[[nodiscard]] ::RenderTexture2D loadHdrTarget(int width, int height, bool withDepth);
void unloadHdrTarget(::RenderTexture2D& target);

class FramePipeline {
 public:
  FramePipeline() = default;
  ~FramePipeline();
  FramePipeline(const FramePipeline&) = delete;
  FramePipeline& operator=(const FramePipeline&) = delete;
  FramePipeline(FramePipeline&&) = delete;
  FramePipeline& operator=(FramePipeline&&) = delete;

  // Redirect subsequent drawing into the HDR scene target, (re)created at the
  // current screen size. Call after BeginDrawing, before ClearBackground/any 3D.
  // On FBO failure degrades to direct backbuffer rendering (warned once) —
  // a diagnostics escape hatch for broken drivers, deliberately linear-looking.
  void begin();
  // Bloom + tonemap + chromatic aberration into a linear LDR target, then
  // `ldrOverlay` (may be empty) draws into that target with raylib 2D/3D calls
  // — X4 draws the non-glow lens-flare elements here, after the tonemap —
  // then the sRGB-encoded frame lands on the backbuffer.
  void end(const std::function<void()>& ldrOverlay = {});

  // Pre-tonemap scene multiplier. 1.0 = X4's units (the captured frame applies
  // none: the LUT samples the HDR buffer directly); the F10 panel moves it.
  void setSceneExposure(float exposure) { sceneExposure_ = exposure; }
  // Diagnostic: after the next end(), write every intermediate target as
  // <prefix>_{scene,ldr,ldr2}.png (linear values clamped, no encode), plus the
  // unclamped <prefix>_scene.raw, so a live-editor artefact can be traced to the pass that
  // introduces it.
  void requestDump(std::string prefix) { dumpPrefix_ = std::move(prefix); }

 private:
  // X4 downsamples from the full-res prefilter to 10x5 at 1440p = 8 levels;
  // the loop also stops when a dimension drops under 4 px.
  static constexpr int kMaxLevels = 8;
  static constexpr int kMsaaSamples = 4;

  [[nodiscard]] bool ensureTargets(int width, int height);
  [[nodiscard]] bool targetsReady() const;
  [[nodiscard]] bool loadMsaaScene(int width, int height);
  void unloadMsaaScene();
  void ensureShaders();
  void ensureLuts();
  void releaseTargets();

  std::string dumpPrefix_;
  // Multisampled FBO the 3D scene draws into; only id and texture.width/height
  // are set (what BeginTextureMode reads). The storage is the multisample colour and depth
  // textures.
  ::RenderTexture2D scene_{};
  unsigned int msaaColor_{0};
  unsigned int msaaDepth_{0};
  ::RenderTexture2D resolved_{};     // scene_ resolved, what every post pass samples
  ::RenderTexture2D prefiltered_{};  // full-res, X4 prefilters before the first downsample
  std::array<::RenderTexture2D, kMaxLevels> down_{};
  std::array<::RenderTexture2D, kMaxLevels> up_{};
  ::RenderTexture2D ldr_{};   // tonemapped, linear
  ::RenderTexture2D ldr2_{};  // after chromatic aberration; overlay target
  int levels_{0};
  int width_{0};
  int height_{0};
  bool active_{false};
  bool warned_{false};
  bool shadersLoaded_{false};
  bool shadersOk_{false};
  ::Shader prefilter_{};
  ::Shader downsample_{};
  ::Shader upsample_{};
  ::Shader tonemap_{};
  ::Shader chroma_{};
  ::Shader encode_{};
  int locDownsampleKaris_{-1};
  int locUpsampleLower_{-1};
  int locTonemapBloom_{-1};
  int locTonemapLut_{-1};
  int locTonemapClut_{-1};
  int locTonemapExposure_{-1};
  unsigned int lut3d_{0};   // fitted X4 tonemap LUT, 32^3 RGB16F, log10-domain coords
  unsigned int clut3d_{0};  // X4 colour-grade CLUT, 32^3 RGB8, gamma-domain coords
  float sceneExposure_{1.0f};
};

}  // namespace x4sb::editor

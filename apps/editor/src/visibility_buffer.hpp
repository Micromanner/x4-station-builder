#pragma once
// Visibility-buffer shading for the opaque station parts (spec 2026-10-14
// visibility-buffer-design): the geometry pass writes (draw | instance, primitive) per
// MSAA sample, then two compute passes shade each pixel once. Classify bins every 8x8
// tile by the materials its samples show; resolve runs one work group per (tile,
// material), so the material's bindless handles are uniform across the group; a
// per-sample raster pass composites the result into the scene target.
// Owns the GL resources and dispatches; the GLSL lives with the other station shaders.
// Needs a live GL context: construct after InitWindow, destroy before CloseWindow.
#include "raylib.h"
#include "rlgl.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace x4sb::editor {

class VisibilityBuffer {
 public:
  // Classify's material bitset covers this many lit materials.
  static constexpr std::size_t kMaxMaterials = 1024;
  // The MSAA sample count the shaders are built for (FramePipeline's).
  static constexpr int kSamples = 4;
  // The compute passes' tile edge (local_size), in pixels.
  static constexpr int kTile = 8;
  // Buffer bindings and the id texture's unit, as the GLSL declares them.
  static constexpr unsigned int kCommandBinding = 7;
  static constexpr unsigned int kVertexBinding = 8;
  static constexpr unsigned int kIndexBinding = 9;
  static constexpr unsigned int kTileBinding = 10;
  static constexpr int kIdUnit = 29;

  VisibilityBuffer(const std::string& classifySource, const std::string& resolveSource);
  ~VisibilityBuffer();
  VisibilityBuffer(const VisibilityBuffer&) = delete;
  VisibilityBuffer& operator=(const VisibilityBuffer&) = delete;
  VisibilityBuffer(VisibilityBuffer&&) = delete;
  VisibilityBuffer& operator=(VisibilityBuffer&&) = delete;

  [[nodiscard]] bool ok() const {
    return classify_ != 0 && resolve_ != 0 && composite_.id != 0 &&
           composite_.id != rlGetShaderIdDefault();
  }
  // The resolve reads the geometry arena as shader storage: no buffer past this fits.
  [[nodiscard]] std::size_t maxStorageBytes() const { return maxStorageBytes_; }
  // Binds the id target, sharing the bound scene framebuffer's depth, and clears it.
  // False (nothing changed) unless that framebuffer is kSamples-multisampled with a
  // depth texture.
  [[nodiscard]] bool begin();
  // The scene framebuffer is bound again.
  void end();
  // Shades every id the geometry pass wrote and composites it into the bound scene
  // target. The caller has bound the station buffers the shaders read; `mvp` is the
  // geometry pass's.
  void resolve(const ::Matrix& mvp);
  // Hi-Z spec phase 0 measurement: marks[baseInstance + instance] = 1 for every
  // visible-list entry that owns a sample. Commands bound at kCommandBinding.
  void markOwners(unsigned int marks);

 private:
  void release();

  unsigned int classify_{0};
  unsigned int resolve_{0};
  unsigned int mark_{0};
  ::Shader composite_{};
  unsigned int emptyVao_{0};  // the composite's vertices come from gl_VertexID
  int mvpLoc_{-1};
  int screenLoc_{-1};
  int samplePosLoc_{-1};
  int classifyScreenLoc_{-1};
  unsigned int scene_{0};  // the framebuffer begin() found bound
  unsigned int depth_{0};  // its depth texture, shared by fbo_
  unsigned int fbo_{0};    // ids + the scene depth
  unsigned int ids_{0};
  unsigned int shaded_{0};  // the resolve's output, per sample
  std::array<int, 2> size_{};
  std::array<float, 8> samplePos_{};  // the sample positions within the pixel
  std::size_t maxStorageBytes_{0};
  unsigned int tiles_{0};
  std::uint32_t tileCapacity_{0};
};

}  // namespace x4sb::editor

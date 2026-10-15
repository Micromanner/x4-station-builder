#pragma once
// CPU mirrors of the station shaders' buffer blocks (design 2026-10-14 §4.4): the
// per-frame FrameData UBO (std140) and the material SSBO records (std430). The
// offsets are pinned below and must match the GLSL in mesh_cache.cpp. Render-free,
// so the dedup table is testable headlessly.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace x4sb::editor {

using Mat4 = std::array<float, 16>;  // column-major, as GL reads it

// kFrameDataGlsl. Defaults match what the station programs saw before their first
// setter call (all terms on, POM on, no cascades, X4's binormal sign).
struct FrameData {
  std::array<Mat4, 5> lightVP{};  // sun shadow cascades
  Mat4 lightView{};               // local-light clustering view
  std::array<float, 3> sunDirection{};
  float sunIntensity{0.0F};
  std::array<float, 3> sunColor{};
  float envMaxLod{0.0F};
  std::array<float, 3> ambientLight{};
  float animTime{0.0F};
  std::array<float, 3> cameraPos{};
  std::int32_t lightMask{15};
  std::array<float, 5> shadowTexel{};
  std::int32_t shadowCascades{0};
  std::int32_t useAo{0};
  std::int32_t usePom{1};
  std::array<float, 2> aoScreen{1.0F, 1.0F};
  std::array<float, 2> clusterViewport{1.0F, 1.0F};
  std::array<float, 2> sliceParams{};
  std::array<float, 2> depthViewport{1.0F, 1.0F};
  std::array<float, 2> depthRange{};
  std::int32_t hasLocalLights{0};
  std::int32_t hasSceneDepth{0};
  std::int32_t normalBinormalSign{-1};
  std::array<std::int32_t, 3> pad{};
};
static_assert(offsetof(FrameData, lightView) == 320);
static_assert(offsetof(FrameData, sunDirection) == 384);
static_assert(offsetof(FrameData, sunColor) == 400);
static_assert(offsetof(FrameData, ambientLight) == 416);
static_assert(offsetof(FrameData, cameraPos) == 432);
static_assert(offsetof(FrameData, lightMask) == 444);
static_assert(offsetof(FrameData, shadowTexel) == 448);
static_assert(offsetof(FrameData, shadowCascades) == 468);
static_assert(offsetof(FrameData, aoScreen) == 480);
static_assert(offsetof(FrameData, depthRange) == 512);
static_assert(offsetof(FrameData, normalBinormalSign) == 528);
static_assert(sizeof(FrameData) == 544);

// One lit hull material (kMaterialGlsl). Handles are bindless texture handles, never
// 0: an absent map holds a 1x1 stand-in, and the int flags (the shader's use*
// switches and enums) gate it. A compiler may sample both sides of a branch, and a
// 0 handle faults the GPU.
struct GpuMaterial {
  std::uint64_t albedoMap{0};
  std::uint64_t metalMap{0};
  std::uint64_t normalMap{0};
  std::uint64_t smoothMap{0};
  std::uint64_t emissiveMap{0};
  std::uint64_t detailDiffuseMap{0};
  std::uint64_t detailNormalMap{0};
  std::uint64_t paintMap{0};
  std::uint64_t dirtMap{0};
  std::uint64_t heightMap{0};
  std::uint64_t glowDetailMap{0};
  std::uint64_t glowDetail2Map{0};
  std::uint64_t multimatDiffuse{0};
  std::uint64_t multimatNormal{0};
  std::uint64_t multimatHeight{0};
  std::array<float, 2> glowAdjust{0.5F, 1.0F};
  // x = metallic, y = 1 - smoothness: X4's scalars, or 1/0 when only a map is
  // given (the map then multiplies in).
  std::array<float, 2> mrFactor{0.0F, 1.0F};
  std::array<float, 2> detailTiling{1.0F, 1.0F};  // x diffuse, y normal
  std::array<float, 2> detailStr{1.0F, 1.0F};     // x diffuse, y normal
  float glowStr{1.0F};
  float diffuseStr{1.0F};
  // Per glow detail layer: UV0 tiling u, v and scroll speed u, v. Defaults =
  // shadergl/ogl/p2_complex_surface.xml.
  std::array<float, 4> glowDetailUv{3.0F, 3.0F, 1.0F, 1.0F};
  std::array<float, 4> glowDetail2Uv{20.0F, 20.0F, 1.0F, 1.0F};
  // Layer strengths, then animglow's baseglow_strength and layerweight.
  std::array<float, 4> glowDetailMix{1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, 3> hullLayerStr{1.0F, 1.0F, 1.0F};  // paint strength, dirt strength, tiling
  float colorHeightCutoff{0.539F};
  std::array<float, 3> pomParams{1.0F, 1.0F, 1.0F};  // height strength, midlevel, aspect ratio
  std::uint32_t pomLayerOffset{0};  // first of 64 layerN_str in the PomLayers buffer
  std::int32_t useSmoothMap{0};
  std::int32_t normalTwoChannel{0};
  std::int32_t useNormalMap{0};
  std::int32_t useMetalMap{0};
  std::int32_t useEmissiveMap{0};
  std::int32_t alphaTest{0};
  std::int32_t additiveUnlit{0};
  std::int32_t opaquePass{1};
  std::int32_t useDiffuseDetail{0};
  std::int32_t useNormalDetail{0};
  std::int32_t usePaint{0};
  std::int32_t useDirt{0};
  std::int32_t hullLayers{0};  // HullLayers
  std::int32_t useHeightMap{0};
  std::int32_t useMultimat{0};  // Multimat
  std::int32_t useMultimatDiffuse{0};
  std::int32_t useMultimatNormal{0};
  std::int32_t useMultimatHeight{0};
  std::int32_t glowDetail{0};  // GlowDetail
  std::int32_t pad{0};
};
static_assert(offsetof(GpuMaterial, glowAdjust) == 120);
static_assert(offsetof(GpuMaterial, glowDetailUv) == 160);
static_assert(offsetof(GpuMaterial, hullLayerStr) == 208);
static_assert(offsetof(GpuMaterial, pomParams) == 224);
static_assert(offsetof(GpuMaterial, useSmoothMap) == 240);
static_assert(offsetof(GpuMaterial, glowDetail) == 312);
static_assert(sizeof(GpuMaterial) == 320);

// One complex_unlit / simple_unlit material (kUnlitMaterialGlsl), handles as in
// GpuMaterial. Defaults: shadergl/ogl/p1_complex_unlit.xml.
struct GpuUnlitMaterial {
  std::uint64_t diffuseMap{0};
  std::array<std::uint64_t, 4> layerMaps{};    // kComplexUnlitLayers order
  std::array<float, 2> softRange{0.0F, 0.1F};  // soft-particle depth band min, max
  std::array<float, 4> diffuseColor{1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, 4> colorEmissive{1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, 4> grade{1.0F, 1.0F, 1.0F, 0.0F};  // brightness, contrast, saturation, hue
  std::array<float, 4> cameraFade{100.0F, 1000.0F, 100000.0F, 120000.0F};  // near, far ranges
  std::array<float, 4> layerStr{1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, 2> angleFade{0.0F, 0.0F};  // speed, offset
  std::array<float, 2> dblend{1.0F, 1.0F};
  float diffuseStr{1.0F};
  float userAlphaScale{1.0F};
  std::int32_t hasDiffuse{0};  // the diffuse map loaded; else diffuseColor stands in
  std::int32_t vertexColors{0};
  std::int32_t isForcefield{0};  // p1_complex_unlit_forcefield
  std::int32_t opaque{0};        // blend NONE: depth-writing, screen-door LOD fade
  std::int32_t layerMask{0};     // bit k = layerMaps[k] loaded
  // 1 xu_lightcone, 2 _env, 3 _alpha; cameraFade then holds X4's near fade range and
  // speed, then the far fade start and stop.
  std::int32_t lightcone{0};
};
static_assert(offsetof(GpuUnlitMaterial, softRange) == 40);
static_assert(offsetof(GpuUnlitMaterial, diffuseColor) == 48);
static_assert(offsetof(GpuUnlitMaterial, angleFade) == 128);
static_assert(offsetof(GpuUnlitMaterial, hasDiffuse) == 152);
static_assert(sizeof(GpuUnlitMaterial) == 176);

// One multi-draw sub-draw's state (kDrawDataGlsl), at drawBase + gl_DrawIDARB.
struct DrawData {
  std::uint32_t materialId{0};      // into the unlit table for complex_unlit ranges
  float lodFade{1.0F};              // LOD cross-fade coverage, 1 = solid
  std::uint32_t flags{0};           // kDraw* bits
  std::uint32_t uvSlot{0};          // texture-scroll transforms; 0 = identity
  std::uint32_t tint{0xFFFFFFFFU};  // editor tint (ghost/selection), RGBA8 (unpackUnorm4x8)
};
static_assert(sizeof(DrawData) == 20);
inline constexpr std::uint32_t kDrawFadeInvert = 1U;  // the complementary dissolve mask
inline constexpr std::uint32_t kDrawStill = 2U;       // animation held at its rest frame
inline constexpr std::uint32_t kDrawHasNormals = 4U;  // the mesh ships XMF normals

// GL's DrawElementsIndirectCommand, as glMultiDrawElementsIndirect reads it.
struct DrawCommand {
  std::uint32_t count{0};
  std::uint32_t instanceCount{0};
  std::uint32_t firstIndex{0};
  std::int32_t baseVertex{0};
  std::uint32_t baseInstance{0};
};
static_assert(sizeof(DrawCommand) == 20);

// Append-only table that stores each distinct value once: equal bytes, same index.
// Every X4 material repeats across the part files and LOD levels that use it.
template <typename T>
class DedupTable {
  // The structs above spell out their padding, so equal values have equal bytes.
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  // Index of `value`, appending it when new.
  [[nodiscard]] std::uint32_t add(const T& value) {
    std::string key(sizeof(T), '\0');
    std::memcpy(key.data(), &value, sizeof(T));
    const auto [it, inserted] = index_.try_emplace(std::move(key), items_.size());
    if (inserted) items_.push_back(value);
    return static_cast<std::uint32_t>(it->second);
  }
  [[nodiscard]] const std::vector<T>& items() const { return items_; }

 private:
  std::vector<T> items_;
  std::unordered_map<std::string, std::size_t> index_;
};

}  // namespace x4sb::editor

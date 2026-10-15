#pragma once
// Background half of native asset streaming: worker threads extract, inflate and
// parse XMF/DDS bytes from the X4 archives. Everything GL (uploads, raylib state,
// logging) stays on the render thread, which drains results via takeCompleted().
#include "x4sb/assetpipe/catalogbuild.hpp"
#include "x4sb/assetpipe/material_library.hpp"
#include "xmf_model.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace x4sb::editor {

struct NativePart {
  std::string xmfPath;
  std::string materialRef;
  bool effect{false};  // MeshRef::effect
};

struct StreamedModel {
  std::string key;
  std::optional<PreparedPart> part;    // nullopt = missing or malformed
  std::vector<MaterialDef> materials;  // one per range
  bool vertexColors{false};            // the XMF ships COLOR[0]
  bool paintUv{false};                 // the XMF ships UV map 2
  bool effect{false};                  // an fx_* part: its non-isPortedFx ranges are not drawn
};

// X4 samples colour maps through sRGB views (station.rdc / X4_Station_FRONT.rdc:
// diffuse, diffuse detail and complex_unlit colour layers bind BC*_SRGB), so the
// GPU decodes before filtering. Data maps stay UNORM. One file can be both.
struct TextureRef {
  std::string path;  // normalizeTexturePath form
  bool srgb{false};
  // Uploads as GL_TEXTURE_2D_ARRAY: the slot decides, so an array file can never bind as 2D.
  bool array{false};
};
// Identifies one upload: a path used both as colour and as data, or both as 2D and as an
// array, is two textures.
[[nodiscard]] std::string textureKey(const TextureRef& ref);

struct StreamedTexture {
  TextureRef ref;
  std::optional<std::string> dds;
};

[[nodiscard]] std::string normalizeTexturePath(const std::string& path);
// X4's layered unlit fx shader: drawn by its own program, which needs UV maps 1-2.
[[nodiscard]] bool isComplexUnlit(const MaterialDef& material);
// complex_unlit's dock-forcefield variant (same inputs, different fade).
[[nodiscard]] bool isForcefield(const MaterialDef& material);
// X4's single-layer unlit shaders (p1_simple_unlit: terminal displays, lava;
// xu_simple_hdr_out: gradient position lights): drawn by the complex_unlit program
// with the extra layers, fades and grade neutral.
[[nodiscard]] bool isSimpleUnlit(const MaterialDef& material);
// X4's xu_lightcone, xu_lightcone_env and xu_lightcone_alpha (shipyard light volumes, haze,
// glass glow): drawn by the complex_unlit program's lightcone branch. The engine and
// animated-tag variants never sit on station modules.
[[nodiscard]] bool isLightcone(const MaterialDef& material);
// The complex_unlit layer maps, in GpuUnlitMaterial::layerMaps order.
inline constexpr std::array<const char*, 4> kComplexUnlitLayers{"diffuse2_map", "diffuse3_map",
                                                                "alpha1_map", "alpha2_map"};
// diffuse2/diffuse3 are colour; the alpha masks are data.
[[nodiscard]] constexpr bool complexUnlitLayerIsColor(std::size_t layer) { return layer < 2; }
// X4's lit hull shaders (p1/p2_complex_surface*, p1/p2_multimat, p2_complex_boron*) and
// projected decals (p1/p2_complex_projection) layer a paint map (UV map 2) and a
// vertex-weighted dirt map over the base material; both colour maps.
[[nodiscard]] bool usesHullLayers(const MaterialDef& material);
// The ranges of an fx_* part we can draw: hull shaders and decals (usesHullLayers,
// decals are ALPHA1NOG cutouts) and the unlit fx programs (lightcone volumes, glows, holo
// panels, xu_lightcone). The rest (distortion, layered smoke, shields) are not ported.
[[nodiscard]] bool isPortedFx(const MaterialDef& material);
inline constexpr std::array<const char*, 2> kHullLayerMaps{"diffuse_paint_map", "color_dirt_map"};
[[nodiscard]] bool isBoron(const MaterialDef& material);
// p2_complex_surface and p2_complex_boron mask their dirt and vertex colour by the POM
// height map (a data map); X4 runs POM by default.
[[nodiscard]] bool usesHeightMask(const MaterialDef& material);
inline constexpr const char* kHeightMap = "height_map";
// p2_complex_boron_animglow: its glow is eaten by two scrolling detail layers (see the shader).
[[nodiscard]] bool isBoronAnimglow(const MaterialDef& material);
// X4's glow detail layers: glow_detail multiplies (or, animglow, cuts into) the glow map and
// is sampled sRGB; animglow's glow_detail2 is sampled linear. Only read beside a glow map.
inline constexpr std::array<const char*, 2> kGlowDetailMaps{"glow_detail_map", "glow_detail2_map"};
[[nodiscard]] constexpr bool glowDetailIsColor(std::size_t map) { return map == 0; }
// X4's multimat shaders pick a layer of 2D-array detail maps per vertex: p1 from DATA1,
// p2 from DATA2 (prepareStationPart's extended ranges).
[[nodiscard]] bool isMultimat(const MaterialDef& material);
[[nodiscard]] bool isP1Multimat(const MaterialDef& material);
[[nodiscard]] bool isP2Multimat(const MaterialDef& material);
// The multimat array maps (diffuse = colour, normal and the POM height = data).
inline constexpr std::array<const char*, 3> kMultimatArrayMaps{
    "diffuse_detail3d_map", "normal_detail3d_map", "height_detail3d_map"};
[[nodiscard]] constexpr bool multimatArrayIsColor(std::size_t map) { return map == 0; }
// Every texture a material samples, normalized; empty paths skipped.
[[nodiscard]] std::vector<TextureRef> materialTextures(const MaterialDef& material);

class NativeStreamer {
 public:
  NativeStreamer(ExtractFn extract, MaterialLibrary materials,
                 std::unordered_map<std::string, NativePart> parts);
  ~NativeStreamer();
  NativeStreamer(const NativeStreamer&) = delete;
  NativeStreamer& operator=(const NativeStreamer&) = delete;
  NativeStreamer(NativeStreamer&&) = delete;
  NativeStreamer& operator=(NativeStreamer&&) = delete;

  // Render thread. The caller dedupes: each key is requested at most once. Each
  // texture a model needs is queued exactly once across all models, so every
  // path in a result's materials eventually arrives as a StreamedTexture.
  // `urgent` jumps the queue (what the user is about to look at).
  void requestModel(const std::string& key, bool urgent = false);

  struct Completed {
    std::vector<StreamedModel> models;
    std::vector<StreamedTexture> textures;
  };
  // Waits up to `wait` for at least one result when none is ready.
  [[nodiscard]] Completed takeCompleted(std::chrono::milliseconds wait = {});

 private:
  struct Job {
    bool texture{};
    std::string name;
    bool srgb{false};   // texture jobs only
    bool array{false};  // texture jobs only
  };
  void run();
  [[nodiscard]] StreamedModel loadModel(const std::string& key);

  const ExtractFn extract_;
  const MaterialLibrary materials_;
  const std::unordered_map<std::string, NativePart> parts_;

  std::mutex mutex_;
  std::condition_variable jobReady_;
  std::condition_variable resultReady_;
  std::deque<Job> jobs_;
  std::unordered_set<std::string> claimedTextures_;
  Completed completed_;
  bool stopping_{false};
  std::vector<std::thread> workers_;
};

}  // namespace x4sb::editor

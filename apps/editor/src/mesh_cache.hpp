#pragma once
// Load-once cache of module parts streamed from the X4 archives into one shared
// GeometryArena. Owns GL resources, so it is non-copyable/non-movable and unloads
// everything in its destructor. Lives only in apps/editor (the sole raylib-linking target);
// libs/* stay render-free. MUST be constructed AFTER InitWindow and destroyed
// BEFORE CloseWindow — uploads and unloads both require a live GL context.
#include "dds_texture.hpp"
#include "geometry_arena.hpp"
#include "gl_ext.hpp"
#include "local_lights.hpp"
#include "native_assets.hpp"
#include "native_stream.hpp"
#include "occlusion_cull.hpp"
#include "raylib.h"
#include "scene_depth.hpp"
#include "ssao.hpp"
#include "station_gpu.hpp"
#include "visibility_buffer.hpp"
#include "x4sb/assetpipe/catalogbuild.hpp"
#include "x4sb/assetpipe/material_library.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/uv_animation.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace x4sb::editor {

// Render treatment of one X4 material, classified from its blendmode
// (spec: 2026-07-10-material-blendmode-design.md).
enum class BlendClass : std::uint8_t {
  Opaque,      // NONE / absent / unknown
  Mask,        // ALPHA1 family: opaque pass + alpha-test discard
  Additive,    // ADDITIVE/SCREEN: transparent pass, additive, UNLIT
  AlphaBlend,  // ALPHA8 family: transparent pass, standard alpha blend, lit
  Skip,        // DISTORTION: meaningless without refraction — never drawn
};

// X4 multimat variant a hull range shades as; the value is the shader's `useMultimat`.
enum class Multimat : std::uint8_t { None, P1, P2 };

// Which shipped shader's paint/dirt chain a range runs; the value is the shader's
// `hullLayers`. P1 = complex_surface and multimat.
enum class HullLayers : std::uint8_t { P1, P2Surface, Boron, P1Projection, P2Projection };

// How a range's glow detail layers apply; the value is the shader's `glowDetail`.
enum class GlowDetail : std::uint8_t { None, Multiply, Animglow };

// Which half of the frame a draw call belongs to: all Opaque+Mask meshes first
// (depth-writing), then Additive+AlphaBlend with depth-write off.
enum class DrawPass : std::uint8_t { Opaque, Transparent };

class MeshCache {
 public:
  // One loaded part file: its XMF ranges in the arena, each with its material.
  struct Part {
    struct Range {
      std::uint32_t firstIndex{0};
      std::uint32_t indexCount{0};
      std::int32_t baseVertex{0};
      std::uint32_t materialId{0};  // into the unlit table when `unlit`, else the lit one
      BlendClass blendClass{BlendClass::Opaque};
      // Drawn with the port of X4's complex_unlit instead of the hull shader.
      bool unlit{false};
      bool twoSided{false};  // X4 TWOSIDED blendmode: opaque, drawn unculled
      bool screen{false};    // an unlit SCREEN range: One/InvSrcColor
      bool glowing{false};   // emits light: emissive map with glow, unlit, or additive
    };
    std::vector<Range> ranges;
    std::array<bool, 2> inPass{};  // some range draws in DrawPass Opaque / Transparent
    bool hasNormals{false};
    std::array<float, 3> boundsMin{};  // part-local, as PreparedPart's
    std::array<float, 3> boundsMax{};
  };
  struct Statistics {
    std::size_t loadedModels{};
    std::size_t missingModels{};
    std::size_t loadedTextures{};
    std::size_t missingTextures{};
    std::size_t pendingModels{};
    bool shadersReady{};
    // The geometry arena's contents (design 2026-10-14 §4.3).
    std::size_t meshRanges{};
    std::size_t uploadedVertexBytes{};
    std::size_t uploadedIndexBytes{};
    std::size_t partVertices{};
    std::size_t partIndices{};
    std::size_t textureBytes{};
    std::size_t materials{};  // distinct lit + unlit materials (design §4.4)
    // Lit-pass split over loaded parts (visibility-buffer spec 2026-10-14 §4 phase 0).
    std::size_t litOpaqueRanges{};
    std::size_t litMaskRanges{};
    std::size_t unlitOpaqueRanges{};
    std::size_t transparentRanges{};
    std::size_t litOpaqueMaterials{};  // distinct
    std::size_t litMaskMaterials{};    // distinct
    std::size_t maxRangeIndices{};     // bounds the primitive id bits
  };
  [[nodiscard]] Statistics statistics() const;
  // Geometry and textures are read straight from the game archives on worker
  // threads (NativeStreamer), which share ownership of the archive. get() never
  // blocks: it queues the part and returns nullptr (drawn as its box) until
  // beginFrame() has uploaded it.
  explicit MeshCache(const NativeAssets& native);
  // Queue without waiting. No-op when cached or pending; `urgent` jumps the queue.
  void request(const std::string& meshKey, bool optional = false, bool urgent = false);
  // Render thread: upload finished work for up to `budgetSeconds` (at least one
  // item when any is ready), optionally waiting briefly for workers first.
  void pumpStreaming(double budgetSeconds, std::chrono::milliseconds wait = {});
  [[nodiscard]] bool isResolved(const std::string& meshKey) const {
    return cache_.count(meshKey) != 0;
  }
  // Grows whenever a load finishes (hit or known-missing): a cheap "what can draw
  // changed" signal for caches of rendered output.
  [[nodiscard]] std::size_t resolvedCount() const { return cache_.size(); }
  ~MeshCache();
  MeshCache(const MeshCache&) = delete;
  MeshCache& operator=(const MeshCache&) = delete;
  MeshCache(MeshCache&&) = delete;
  MeshCache& operator=(MeshCache&&) = delete;

  // Load-once, cached, keyed by the catalog's MeshRef key. Returns the part, or
  // nullptr while it streams in or when it is missing/malformed (negative-cached,
  // never retried). The returned pointer stays valid for the lifetime of the
  // cache (unordered_map node addresses are stable). `optional` marks a key whose
  // absence is expected (a coarser-LOD sibling most parts don't ship) so it is
  // negative-cached without the missing-mesh warning.
  [[nodiscard]] const Part* get(const std::string& meshKey, bool optional = false);

  // Like get() but blocks until the part is resolved (harnesses, validation).
  const Part* warm(const std::string& meshKey, bool optional = false);

  // Call once at the top of each frame: uploads finished background work on a
  // small time budget so an import never stalls one frame (known-issues 1.3).
  void beginFrame();

  void setLighting(::Vector3 sunDir, ::Color sunColor, float sunIntensity, ::Color ambientColor,
                   float ambientIntensity, ::Vector3 camPos);
  // Debug isolation of the hull lighting terms (kFlatFsBody `lightMask`): bit0 sun
  // diffuse, bit1 sun specular, bit2 IBL env reflection, bit3 local lights. Default = all on.
  void setLightMask(int mask);
  // Masks from 32 up dump material terms compared against X4's G-buffer, which holds no
  // transparent fx: the transparent pass is skipped for them.
  [[nodiscard]] bool materialTermActive() const { return frame_.lightMask >= 32; }
  // The frame's authored station lights: begin/add on localLights(), then
  // commitLocalLights() before the hull draws that should see them.
  LocalLights& localLights() { return localLights_; }
  void commitLocalLights();
  // Snapshot the scene depth for the transparent fx that follow (forcefields).
  void captureSceneDepth();
  // The copy captureSceneDepth took this frame; nullptr when there is none.
  [[nodiscard]] const SceneDepthCopy* sceneDepth() const {
    return hasSceneDepth_ ? &sceneDepth_ : nullptr;
  }

  // Binds the baked sky cubemap (EnvCubemap, or the 1x1 black fallback) for the
  // IBL terms in kFlatFsBody. `maxLod` is the deepest mip index (irradiance level).
  void setEnvMap(unsigned int cubemapId, float maxLod);

  // Sun shadow cascade `index` (SunShadow renders it, depth-compare texture):
  // persistent bind + the light view-projection; texelUv sizes the PCF kernel.
  void setShadowCascade(int index, unsigned int depthTexId, const ::Matrix& lightVP, float texelUv);
  // Bit i set = cascade i is valid this frame; 0 = no sun shadow.
  void setShadowCascades(int mask);
  // While set, drawInstanced emits depth only (opaque classes, no shading): the
  // shadow pass body.
  void setShadowPass(bool on) { shadowPass_ = on; }
  // While set, drawInstanced writes depth only for the parts the lit pass will
  // draw at the same depth (solid and not complex_unlit): the SSAO input. With the
  // visibility buffer active it is also its geometry pass (ids, see resolveVisibility).
  void setDepthPrepass(bool on);
  // The opaque draws that follow were not in the prepass, so they write their own depth
  // (and are shaded forward).
  void endPrepassDepth() {
    prepassDepthValid_ = false;
    visibilityResolved_ = false;
  }
  // When the prepass ran as the visibility buffer's geometry pass: shades its ids into
  // the colour target, and the opaque lit draws that follow skip those ranges. Call
  // after computeSsao, before any other drawInstanced.
  void resolveVisibility();
  // Hi-Z spec phase 0 (X4SB_OCCLUSION_STATS=1, stalls): the last geometry pass's
  // submitted vs sample-owning work.
  struct OcclusionStats {
    std::uint64_t entries{};
    std::uint64_t visibleEntries{};
    std::uint64_t indices{};
    std::uint64_t visibleIndices{};
    std::uint64_t preCullIndices{};  // before the occlusion cull (= indices without it)
  };
  [[nodiscard]] const OcclusionStats& occlusionStats() const { return occlusionStats_; }
  // SSAO from the prepass depth; the hull draws that follow apply it until
  // setAoEnabled(false) (transparent fx and the selected module never do).
  void computeSsao();
  void setAoEnabled(bool enabled);
  // Off skips the SSAO pass entirely; computeSsao then leaves AO unapplied.
  void setSsaoEnabled(bool enabled) { ssaoEnabled_ = enabled; }
  // X4's POM on the height-mapped hull materials, on by default like the game's "high".
  void setPomEnabled(bool enabled);
  // Normal-map binormal sign; -1 matches the measured X4 convention. Viewshots can A/B it.
  void setNormalBinormalSign(int sign);
  // Clock for the anim_* texture scroll; 0 (the default) is a still frame.
  void setAnimationTime(double seconds) {
    animTime_ = seconds;
    frame_.animTime = static_cast<float>(seconds);
    frameDirty_ = true;
  }
  [[nodiscard]] bool isAnimated(const std::string& meshKey) const {
    return uvAnimations_.count(meshKey) != 0;
  }
  // One part's drawable file per LOD level 0-3: an unshipped level falls back
  // toward lod0, nullptr = nothing drawable (yet). Memoized once every level has
  // finished loading; building and looking up these keys per part per frame was
  // most of the instance-collection cost (known-issues 1.5). The pointers are
  // cache keys, stable for the cache's life. `ref` must outlive the cache (the
  // catalog's refs do).
  struct PartFiles {
    std::array<const std::string*, 4> lod{};
    bool animated{false};
  };
  [[nodiscard]] const PartFiles& partFiles(const MeshRef& ref);

  // The instance matrices every draw indexes through its visible list (design
  // 2026-10-14 §4.5): the station's parts, one per (placed module, mesh ref) in
  // station order, re-uploaded only when the station revision changes; then the
  // transient slots, each one module's worth of parts.
  [[nodiscard]] bool stationInstancesCurrent(std::uint64_t revision) const {
    return instancesRevision_ == revision;
  }
  void uploadStationInstances(std::uint64_t revision, const std::vector<::Matrix>& xforms);
  // Modules drawn at a pose the station doesn't hold: the selection's drag preview, the ghost.
  enum class TransientSlot : std::uint8_t { Selection, Ghost };
  // Writes `xforms` (at most one module's parts) to `slot`; returns their first instance
  // index, nullopt before the station instances exist or past the slot size.
  [[nodiscard]] std::optional<std::uint32_t> uploadTransientInstances(
      TransientSlot slot, const std::vector<::Matrix>& xforms);
  // One part file's slice of an uploadVisible batch, for the occlusion cull.
  struct VisibleBucket {
    const std::string* meshKey{nullptr};
    std::size_t first{0};
    std::uint32_t count{0};
  };
  // Upload the visible lists (instance indices) for the drawInstanced calls that
  // follow in one buffer write; each draw then reads its [first, first+count) range.
  // With `cullBuckets` (the main view) the depth prepass culls by occlusion and every
  // later draw of the batch reads the survivors (hiz spec 2026-10-15 §2).
  void uploadVisible(const std::vector<std::uint32_t>& instances,
                     const std::vector<VisibleBucket>& cullBuckets = {});
  // Called as each step of the culled prepass ends (--profile's GPU marks).
  enum class CullStep : std::uint8_t { PhaseA, Hiz, Cull };
  void setCullStepHook(std::function<void(CullStep)> hook) { cullStepHook_ = std::move(hook); }
  // Every instance of one cached part file, drawn at one LOD fade step.
  struct InstancedDraw {
    const std::string* meshKey{nullptr};
    std::size_t first{0};  // into the last uploadVisible() batch
    std::uint32_t count{0};
    // Screen-door coverage in (0,1], 1 = solid; `invert` takes the complementary
    // mask, so the outgoing and incoming level of a part tile every pixel once.
    float fade{1.0F};
    bool invert{false};
    bool still{false};        // texture scroll held at its rest frame (small on screen)
    std::uint32_t bucket{0};  // index into the batch's cullBuckets
  };
  // One pass's draws, tinted with `tint`: one multi-draw per state class (design
  // 2026-10-14 §4.6), every range of every listed part a sub-draw. Parts still
  // streaming or missing are skipped (the caller boxes them).
  void drawInstanced(const std::vector<InstancedDraw>& draws, ::Color tint, DrawPass pass);

 private:
  // Materials of a loaded mesh that emit light (emissive map with glow, unlit, additive).
  [[nodiscard]] int glowingMaterials(const std::string& meshKey) const;
  // GetShaderLocation is a glGetUniformLocation driver lookup; drawBase is looked up
  // once per multi-draw.
  [[nodiscard]] int uniformLoc(const ::Shader& shader, std::string_view name) const;
  struct NameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
  };
  using LocTable = std::unordered_map<std::string, int, NameHash, std::equal_to<>>;
  mutable std::unordered_map<unsigned int, LocTable> uniformLocs_;  // by shader program id

  // Last value per (program, location) written through setUniform: passes mostly
  // repeat the tint and mvp, and each glUseProgram + glUniform* pair is driver time.
  // A location set through it must never also be set around it (SetShaderValue), or
  // the cache goes stale. setUniform takes a scalar (int/float) or a vec4.
  void setUniform(const ::Shader& shader, int loc, const void* value, int type) const;
  void setUniformMatrix(const ::Shader& shader, int loc, const ::Matrix& m) const;
  struct UniformValue {
    std::array<unsigned char, 64> bytes{};
    std::size_t size{0};  // 0 = never written through the cache
  };
  mutable std::unordered_map<unsigned int, std::vector<UniformValue>> uniformValues_;

  // Flushes raylib's pending batch, uploads the frame data and materials that
  // changed, binds the buffers and sets the pass's view-projection.
  void beginInstancedDraws();
  [[nodiscard]] static ::Matrix passMvp();
  // drawInstanced's state classes, in submission order: each is one multi-draw
  // with one program and one blend/depth state.
  enum class DrawClass : std::uint8_t {
    Depth,
    Prepass,
    PrepassMask,  // the visibility pass's cutouts
    LitOpaque,
    LitMask,
    UnlitOpaque,
    LitAlphaBlend,
    LitAdditive,
    UnlitBlended,
    UnlitScreen,      // X4 SCREEN lightcones: One/InvSrcColor
    UnlitAlphaBlend,  // xu_lightcone_alpha
    Count,
  };
  static constexpr std::size_t kDrawClasses = static_cast<std::size_t>(DrawClass::Count);
  [[nodiscard]] std::optional<DrawClass> drawClassOf(const Part::Range& range, DrawPass pass) const;
  void submitClass(DrawClass cls, std::size_t base, std::size_t count);
  // drawInstanced's per-class sub-draws, reused across calls.
  std::array<std::vector<DrawCommand>, kDrawClasses> classCommands_;
  std::array<std::vector<DrawData>, kDrawClasses> classDrawData_;
  std::vector<DrawCommand> commands_;
  std::vector<DrawData> drawData_;
  std::vector<UvTransforms> uvSlots_;
  unsigned int indirectBuffer_{0};
  unsigned int drawDataSsbo_{0};
  unsigned int uvAnimSsbo_{0};
  GeometryArena arena_;
  unsigned int instanceSsbo_{0};
  unsigned int visibleSsbo_{0};
  std::size_t visibleCount_{0};               // the CPU lists' length (one region when culling)
  std::unique_ptr<OcclusionCull> occlusion_;  // null under X4SB_NO_OCCLUSION=1
  bool cullBatch_{false};        // the current batch culls (uploadVisible with buckets)
  bool cullDone_{false};         // and its phase B ran: later draws read kVisible
  bool cullHistoryStale_{true};  // the instance ids moved since the last cull
  std::function<void(CullStep)> cullStepHook_;
  bool occlusionStatsOn_{false};
  unsigned int marksSsbo_{0};
  OcclusionStats occlusionStats_{};
  std::optional<std::uint64_t> instancesRevision_;
  std::size_t stationInstances_{0};
  std::size_t transientSlotSize_{0};  // the catalog's most parts in one module
  std::vector<Mat4> instanceScratch_;
  void packInstances(const std::vector<::Matrix>& xforms);  // into instanceScratch_

  struct PartEntry {
    PartFiles files;
    bool settled{false};  // every level loaded or known-missing: files are final
  };
  std::unordered_map<const MeshRef*, PartEntry> partFiles_;

  // Shared body of get()/warm(): cache lookup, else queue; warm() waits.
  const Part* tryLoad(const std::string& meshKey, bool budgeted, bool optional);
  void finishNative(StreamedModel&& streamed);
  void uploadNativeTexture(StreamedTexture&& streamed);

  // The complex_unlit record of `material`; call once its textures are resident.
  [[nodiscard]] GpuUnlitMaterial complexUnlitInfo(const MaterialDef& material, bool vertexColors);
  // The resident bindless handle of texture `id`, created on first use; 0 for 0.
  [[nodiscard]] std::uint64_t textureHandle(unsigned int id);
  // A material's handle for 2D / array texture `id`: 0 (absent) maps to the 1x1
  // stand-in (raylib's white texture / fallbackArray_), never to a 0 handle.
  [[nodiscard]] std::uint64_t handle2D(unsigned int id);
  [[nodiscard]] std::uint64_t handleArray(unsigned int id);

  std::unique_ptr<NativeStreamer> streamer_;
  // Requested but not yet in cache_; value = optional (absence expected, no warning).
  std::unordered_map<std::string, bool> pendingNative_;
  std::deque<StreamedTexture> texturesToUpload_;
  // Parsed models waiting for every texture they sample to be resident (or failed).
  std::vector<StreamedModel> modelsAwaitingTextures_;
  std::unordered_set<unsigned int> bc5Textures_;
  Statistics uploaded_;  // only the byte/count tallies are used
  std::unordered_map<std::string, std::optional<Part>> cache_;  // nullopt = known-missing
  // One GPU texture per normalized archive path, shared by every model's
  // materials; unloaded (once) in the destructor.
  std::unordered_map<std::string, ::Texture2D> texturesByUri_;
  // The TextureRef::array uploads, apart so no 2D slot can resolve to one.
  std::unordered_map<std::string, DdsArrayTexture> arrayTexturesByUri_;
  // Every part's materials, deduplicated (design §4.4), and the multimat height
  // strengths their pomLayerOffset points into; uploaded by beginInstancedDraws.
  DedupTable<GpuMaterial> materials_;
  DedupTable<GpuUnlitMaterial> unlitMaterials_;
  DedupTable<std::array<float, 64>> pomLayers_;
  bool materialsDirty_{false};
  unsigned int materialSsbo_{0};
  unsigned int pomLayerSsbo_{0};
  unsigned int unlitMaterialSsbo_{0};
  GlExtensions gl_;
  std::unordered_map<unsigned int, std::uint64_t> textureHandles_;  // by texture id
  FrameData frame_;
  bool frameDirty_{true};
  unsigned int frameUbo_{0};
  // Texture-scroll curves of the animated parts, under the lod0 and every lodN key.
  std::unordered_map<std::string, UvAnimation> uvAnimations_;
  double animTime_{0.0};
  LocalLights localLights_;
  SceneDepthCopy sceneDepth_;
  bool hasSceneDepth_{false};
  Ssao ssao_;
  bool ssaoEnabled_{true};
  bool depthPrepass_{false};
  // The opaque buckets' depth is already in the buffer (prepass ran this frame).
  bool prepassDepthValid_{false};
  // The lit station shader: reads each instance's model matrix from the instance
  // buffer through its sub-draw's visible list, so one sub-draw covers every copy of a range.
  ::Shader instancedShader_{};
  // X4's complex_unlit for station parts.
  ::Shader complexUnlitShader_{};
  ::Shader depthInstancedShader_{};
  ::Shader depthPrepassShader_{};
  // The prepass as the visibility buffer's geometry pass (ids, no shading).
  ::Shader visibilityShader_{};
  ::Shader visibilityMaskShader_{};               // its cutouts
  std::unique_ptr<VisibilityBuffer> visibility_;  // null under X4SB_FORWARD=1
  bool visibilityPass_{false};      // this prepass writes ids, resolveVisibility shades them
  bool visibilityResolved_{false};  // the prepass's ranges are shaded already
  // Every station program compiled; else no part draws as a mesh (all box).
  bool shadersOk_{false};
  bool shadowPass_{false};
  // 1x1 black cubemap bound at construction so the shader's samplerCube is never
  // undefined before the first real setEnvMap() call (see the ctor + setEnvMap).
  unsigned int fallbackEnv_ = 0;
  unsigned int fallbackArray_ = 0;  // 1x1x1 white GL_TEXTURE_2D_ARRAY, see handleArray
  // Re-bound by beginFrame(): the planet pass binds up to 13 texture units and
  // unbinds them all, which zeroes unit 10 between SunShadow and the hull draws.
  std::array<unsigned int, 5> shadowTex_{};
  void bindShadowMap() const;
};

}  // namespace x4sb::editor

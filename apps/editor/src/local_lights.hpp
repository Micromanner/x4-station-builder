#pragma once
// X4's authored station lights (component <lights>) for the forward hull shader.
// X4 draws them as deferred light volumes; with no G-buffer here they are binned
// per frame into a clustered grid (screen tiles x log-depth slices) that kFlatFsBody
// walks. Light records, per-cluster ranges and the index list ride texture
// buffers on units 12-14 (kFlatFsBody binds its samplers there).
// Needs a live GL context: construct after InitWindow, destroy before CloseWindow.
#include "raylib.h"
#include "station_gpu.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/types.hpp"
#include "x4sb/document/station.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace x4sb::editor {

class LocalLights {
 public:
  LocalLights() = default;
  ~LocalLights();
  LocalLights(const LocalLights&) = delete;
  LocalLights& operator=(const LocalLights&) = delete;
  LocalLights(LocalLights&&) = delete;
  LocalLights& operator=(LocalLights&&) = delete;

  // Per frame, inside BeginMode3D (reads rlgl's view/projection and viewport):
  // begin, addStation, add for a module posed away from the station (the drag
  // preview), then commit before the draws.
  void begin();
  // Every placed module's lights except `skip`'s. The world-space records are
  // cached until the station changes; only the view cull and fade run per frame.
  void addStation(const Station& station, const ModuleCatalog& catalog,
                  std::optional<InstanceId> skip);
  void add(const ModuleDef& def, const Transform& world);
  void commit();
  // The committed grid's view, viewport and slicing for the station shaders.
  void apply(FrameData& frame) const;

 private:
  static constexpr std::size_t kTexelsPerLight = 6;
  // One light in display space: everything derived from the module pose, with
  // the colour texel (1) before the per-view fade.
  struct WorldLight {
    std::array<std::array<float, 4>, kTexelsPerLight> texels{};
    Vec3 pos{};
    double radius{0.0};
    double lodRadius{0.0};
    double fullPx{0.0};  // projected lodRadius at which it is fully faded in
  };
  static void worldLights(const ModuleDef& def, const Transform& world,
                          std::vector<WorldLight>& out);
  void push(const WorldLight& light);  // view cull + fade into this frame's records

  struct Buffer {
    unsigned int buffer{0};
    unsigned int texture{0};
  };
  void upload(Buffer& b, unsigned int internalFormat, const void* data, std::size_t bytes);

  ::Matrix view_{};
  ::Matrix proj_{};
  std::array<float, 2> viewport_{1.0F, 1.0F};
  float projF_{1.0F};  // projected 720p-reference pixels per unit radius at unit depth
  std::array<float, 2> sliceParams_{};
  std::array<std::array<double, 3>, 4> sidePlanes_{};  // view frustum sides, unit normals
  std::uint64_t stationRevision_{0};
  std::vector<WorldLight> stationLights_;
  // Per placed module (station order): its instance id and first stationLights_ index.
  std::vector<std::pair<InstanceId, std::size_t>> moduleLights_;
  std::vector<WorldLight> scratch_;
  std::vector<std::array<float, 4>> records_;  // kTexelsPerLight texels per light
  std::vector<double> radii_;                  // bounding sphere per light
  std::vector<std::array<std::uint32_t, 2>> clusters_;
  // (cluster, light) scratch, one list per binning worker (in light order).
  std::vector<std::vector<std::array<std::uint32_t, 2>>> pairs_;
  std::vector<std::uint32_t> indices_;
  std::size_t lightCount_{0};
  Buffer data_;
  Buffer grid_;
  Buffer index_;
};

}  // namespace x4sb::editor

#pragma once
// Offline extraction of X4 Cluster Backgrounds (spec phase 2).
#include "x4sb/assetpipe/component.hpp"
#include "x4sb/assetpipe/lensfx.hpp"
#include "x4sb/assetpipe/nebula_fog.hpp"
#include "x4sb/data/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace x4sb {
class Archive;
class MaterialLibrary;

namespace assetpipe {

struct RgbColor {
  uint8_t r, g, b, a;
};

struct Star {
  Vec3 dir;
  RgbColor color;
  float brightness;
  // p1_star instance data (captured 2026-09-19): `range` is the sprite radius
  // (quad height = 16*range/1080 screen heights), `specularintensity` the
  // 8-column atlas offset (0, 0.125, ..., 0.875).
  float range{1.0F};
  float specularIntensity{0.0F};
};

// The star sprite atlas every cluster shares (engine-bound, not in any material).
inline constexpr const char* kStarAtlasSource =
    "assets/textures/environments/star_sprites_01_diffhq";

struct Sun {
  Vec3 dir;
  RgbColor color;
  float intensity;
  std::string lenseffect;
  double distance{0.0};  // |omni offset| in metres; 0 = unknown (angular sun
                         // element sizes cannot be derived without it)
};

// The cluster's <ambient> light (planet bodies read it raw as V_ambient1).
struct Ambient {
  RgbColor color{0, 0, 0, 255};
  float intensity{0.0F};
};

struct ClusterBackground {
  std::vector<Star> stars;
  Sun sun{};
  Ambient ambient{};
};

// `prefix` is the archive source that ships the cluster ("" = base game,
// "extensions/<dlc>/" for DLC clusters — their XMLs live under the extension's
// logical prefix). The sun is seen from `sectorOffset` (cluster space, X4 axes):
// sectors sit up to 2e8 m from the cluster origin, shifting the sun up to 30 deg.
[[nodiscard]] std::optional<ClusterBackground> extractClusterBackground(
    const Archive& archive, int clusterId, const std::string& prefix = "", Vec3 sectorOffset = {});

// One sky part of a cluster (a sky sphere, extent >= ~5e8 m, or a smaller nebula sheet
// placed far out), drawn as camera-locked sky geometry. X4 places it in cluster space,
// so a sector up to ~1e8 m off the cluster origin sees it with parallax (havtika choice
// 3.rdc: several degrees). pos/quat are already display-space (the (1,1,-1)
// X4->display flip is applied at extraction, matching the manifest's flipped star dirs).
struct ClusterDome {
  std::string part;
  std::string xmfPath;  // archive address; filled by the caller (needs the source prefix)
  std::string materialRef;
  bool additive{false};  // material blendmode ADDITIVE (star spheres) vs opaque-unlit
  Vec3 pos;
  Quat quat;
  double extent{0};  // largest <size><max> component; the editor scales by radius/(|pos|+extent)
};

// Sky parts from a cluster component's parts: extent >= 1e8 is the property that
// makes a part sky (station parts sit far below), plus parts named as sky
// (bg/nebula/background): Cluster_15's nebula001 is 9.8e7, while lowering the
// cutoff would admit non-sky parts like Cluster_50's fogwall. Planet-system parts are
// excluded even when that large (see selectPlanetParts; with no library nothing
// can be classified, so nothing is excluded). Positions are relative to
// `sectorOffset` (cluster space, X4 axes).
[[nodiscard]] std::vector<ClusterDome> selectSkyDomes(const ComponentGeometry& geo,
                                                      const MaterialLibrary* matLib,
                                                      Vec3 sectorOffset = {});

enum class PlanetPartKind : std::uint8_t { Planet, Moon, Atmosphere, Ring, Aurora, Sun, Flare };

// One planet-system part (docs/2026-09-23-planet-rendering-investigation.md).
// Classified by material shader, never by part name: cluster_03's "planet" is a
// moon and cluster_29's ring is "Tube01".
struct PlanetPart {
  std::string part;
  std::string xmfPath;  // archive address; filled by the caller (needs the source prefix)
  std::string materialRef;
  PlanetPartKind kind{PlanetPartKind::Planet};
  // Index (into the same list) of the planet/moon whose connection position this
  // atmosphere/ring/aurora shares; -1 for bodies and unpaired strays.
  int body{-1};
  Vec3 pos;  // display space, relative to the viewing sector
  Quat quat;
  double radius{0};         // metres; the body itself for orbiting moons (size_raw)
  UvAnimation uvAnimation;  // surface/band scroll (Jupiter); empty for most parts
};

// Planet bodies, atmospheres, rings, auroras and sun bodies of a cluster, seen
// from `sectorOffset` (cluster space, X4 axes). Rings are the ALPHA8
// p1_complex_unlit parts sharing a body's position; other ALPHA8 scenery
// (volcanic smoke) is not a ring. Flares are the ADDITIVE p1_complex_unlit parts
// sharing a body's or sun's position (cluster_05's sun_flares_v3 lofts).
[[nodiscard]] std::vector<PlanetPart> selectPlanetParts(const ComponentGeometry& geo,
                                                        const MaterialLibrary& matLib,
                                                        Vec3 sectorOffset = {});

// Serialize the background manifest the editor's Sky loads; every texture and
// mesh field is a game-archive address. Planes with angular size > 3 are omitted
// (they are dome meshes). Non-empty `sunSprites` adds sun.lenseffect/distance/
// elements (approach-B sun spec); a fog field with spheres adds "fog".
[[nodiscard]] std::string backgroundManifestJson(const ClusterBackground& bg,
                                                 const std::vector<ClusterDome>& domes,
                                                 const std::vector<SunSprite>& sunSprites,
                                                 const std::vector<PlanetPart>& planets = {},
                                                 const FogField& fog = {});

}  // namespace assetpipe
}  // namespace x4sb

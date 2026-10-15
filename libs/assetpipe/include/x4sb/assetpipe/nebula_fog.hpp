#pragma once
// X4's volumetric nebula fog, data side: the regions a cluster places
// (clusters.xml), their boundaries and <volumetricfog> fields
// (region_definitions.xml) and the participating media (medium_library.xml),
// turned into the spheres X4's fog pass draws. Capture facts and the calibration
// behind the constants: docs/2026-10-11-nebula-fog-findings.md.
#include "x4sb/data/math.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace x4sb::assetpipe {

// X4 binds at most three media per frame (I_volume_bind_index[3]), each with at
// most three phase functions (V_volume_phase0..2_param).
inline constexpr std::size_t kMaxFogMedia = 3;
inline constexpr std::size_t kMaxFogPhases = 3;
// Captured sphere radius / volumetricfog size (both cluster_14 fields).
inline constexpr double kFogRadiusScale = 0.86576;
// The nebulafar lodrule (renderparam_library.xml): spheres beyond it are not drawn.
inline constexpr double kFogRange = 600000.0;
// Hostile-input cap on generated or loaded spheres.
inline constexpr std::size_t kMaxFogSpheres = 16384;

enum class PhaseFunction : std::uint8_t {
  Isotropic,
  Rayleigh,
  HenyeyGreenstein,
  MieHazy,
  MieMurky
};

struct FogPhase {
  PhaseFunction function{PhaseFunction::Isotropic};
  std::array<float, 3> sigmaS{};  // scattering coefficient per metre
  float shape{0.0F};              // HG g
};

struct FogMedium {
  std::string name;
  std::string pattern;  // archive address of the triplanar density texture
  std::array<float, 3> sigmaExt{};
  std::vector<FogPhase> phases;  // <= kMaxFogPhases
};

struct FogSphere {
  Vec3 pos;  // cluster space, X4 axes
  double radius{0.0};
  float density{0.0F};  // the field's multiplier
  int medium{0};        // index into FogField::media
};

struct FogField {
  Vec3 origin;  // where the station sits, cluster space
  std::vector<FogMedium> media;
  std::vector<FogSphere> spheres;
};

struct SplinePoint {
  Vec3 pos;
  Vec3 tangent;
  double inLength{0.0};
  double outLength{0.0};
};

struct FalloffStep {
  double position{0.0};
  double value{0.0};
};

struct VolumetricFogDef {
  std::string medium;
  std::string texture;
  double multiplier{0.0};
  double size{0.0};
  double sizeVariation{0.0};
  std::uint32_t seed{0};
};

// Cylinder axis is local y, `linear` its full length; box sizes are full edge
// lengths (both assumed: no capture covers them). Unbounded = no <boundary>,
// cluster-wide, generated out to kFogRange around the placement.
enum class RegionBoundary : std::uint8_t { Sphere, SplineTube, Cylinder, Box, Unbounded };

struct RegionDef {
  std::string name;
  RegionBoundary boundary{RegionBoundary::Sphere};
  double radius{0.0};
  double linear{0.0};
  Vec3 box;
  std::vector<SplinePoint> spline;
  double density{1.0};
  double minNoise{0.0};
  double maxNoise{1.0};
  std::vector<FalloffStep> lateral;  // along the spline / cylinder axis, normalised
  std::vector<FalloffStep> radial;   // across the boundary, normalised radius
  std::vector<VolumetricFogDef> fogs;
};

struct RegionLibrary {
  std::unordered_map<std::string, RegionDef> regions;
  std::unordered_map<std::string, std::string> aliases;
  // Follows alias chains; nullptr when unknown or cyclic.
  [[nodiscard]] const RegionDef* find(std::string_view name) const;
};

struct RegionPlacement {
  std::string region;
  Vec3 offset;
  Quat rotation;
};

using MediumLibrary = std::unordered_map<std::string, FogMedium>;

// The region connections of `clusterMacro` in a clusters.xml-style macro file.
[[nodiscard]] std::vector<RegionPlacement> parseClusterRegions(std::string_view clustersXml,
                                                               std::string_view clusterMacro);
// Merge one region_definitions.xml into `into`; a later document wins by name
// (the DLCs ship whole documents, not diffs).
void parseRegionDefinitions(std::string_view xml, RegionLibrary& into);
void parseMediumLibrary(std::string_view xml, MediumLibrary& into);

// Cubic Bezier between two spline points: handles along the tangents (unit
// after parsing), out/in lengths long.
[[nodiscard]] Vec3 bezierPoint(const SplinePoint& a, const SplinePoint& b, double t);
// A field's spheres in the region's own frame (before the placement).
[[nodiscard]] std::vector<FogSphere> generateFogSpheres(const RegionDef& region,
                                                        const VolumetricFogDef& fog);
// Every fog field of the placed regions, media grouped in first-seen order
// (fields past kMaxFogMedia media are dropped).
[[nodiscard]] FogField buildFogField(const std::vector<RegionPlacement>& placements,
                                     const RegionLibrary& regions, const MediumLibrary& media,
                                     Vec3 origin);

// Background manifest round-trip. Reading validates: spheres with bad numbers or
// unknown media are dropped, extra media and phases truncated.
[[nodiscard]] nlohmann::json fogFieldToJson(const FogField& field);
[[nodiscard]] FogField fogFieldFromJson(const nlohmann::json& j);
// An exact sphere list ({"spheres":[...]} as written by tools/fog_capture_spheres.py)
// mapped onto `media` by name; unknown media are dropped.
[[nodiscard]] std::vector<FogSphere> fogSpheresFromJson(const nlohmann::json& j,
                                                        const std::vector<FogMedium>& media);

}  // namespace x4sb::assetpipe

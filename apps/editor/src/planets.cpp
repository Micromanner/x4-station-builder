#include "planets.hpp"

#include "dds_texture.hpp"
#include "external/glad.h"
#include "native_assets.hpp"
#include "planet_shaders.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "x4sb/assetpipe/native_background.hpp"
#include "x4sb/assetpipe/xmf.hpp"
#include "xmf_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>

namespace x4sb::editor {
namespace {

// F_globallightscale as bound in argon_prime_planet.rdc.
constexpr float kGlobalLightScale = 1.0F;

struct FloatParam {
  const char* uniform;  // "S_"/"U_" + the lower-cased material property name
  float fallback;       // shadergl/ogl/p1_*.xml default
};

struct ColorParam {
  const char* uniform;
  std::array<float, 4> fallback;
};

struct ProgramSpec {
  const char* vs;
  std::string fs;
  std::vector<FloatParam> floats;
  std::vector<ColorParam> colors;
  std::vector<const char*> samplers;  // material property names ("diffuse_map")
  // Shaders without SHADERFLAG_DONT_DECODE_SRGB sample through sRGB views and
  // get sRGB-decoded Color props (the p1_simple_unlit capture's color_emissive).
  bool srgb;
};

std::vector<FloatParam> scatteringParams(float outer, float inner, float esun) {
  return {{"U_wavelength_red", 0.65F},   {"U_wavelength_green", 0.57F},
          {"U_wavelength_blue", 0.475F}, {"U_fouterradius", outer},
          {"U_finnerradius", inner},     {"U_fdensity", 1.0F},
          {"U_fscaledepth", 0.25F},      {"U_m_esun", esun},
          {"U_m_kr", 0.0025F},           {"U_m_km", 0.0010F}};
}

std::vector<FloatParam> gradeParams() {
  return {{"U_base_brightness_shift", 1.0F},
          {"U_base_contrast_shift", 1.0F},
          {"U_base_saturation_shift", 1.0F},
          {"U_base_hue_shift", 0.0F}};
}

// A ring mesh's local normal: the thinnest axis of its bounds.
::Vector3 ringAxis(const ::Model& model) {
  const ::BoundingBox box = GetModelBoundingBox(model);
  const ::Vector3 size = Vector3Subtract(box.max, box.min);
  if (size.x <= size.y && size.x <= size.z) return {1.0F, 0.0F, 0.0F};
  if (size.y <= size.z) return {0.0F, 1.0F, 0.0F};
  return {0.0F, 0.0F, 1.0F};
}

std::vector<FloatParam> join(std::vector<FloatParam> a, const std::vector<FloatParam>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// `define` selects a variant of a shared port (planet_shaders.hpp).
std::string bodyFs(const char* body, const char* define = "") {
  return std::string(planet_glsl::kBodyFsHeader) + define + planet_glsl::kSharedFs +
         planet_glsl::kBodyFsHelpers + body;
}

std::string complexUnlitFs() {
  return std::string(planet_glsl::kComplexUnlitFsHeader) + planet_glsl::kSharedFs +
         planet_glsl::kComplexUnlitFsMain;
}

// planet_earthlike and planet_xenon; they differ only in the cloud_speed default.
std::vector<FloatParam> earthlikeFloats(float cloudSpeed) {
  return join(join({{"S_diffusestr", 1.0F},
                    {"U_diffoffsetu", 0.0F},
                    {"S_diffuse_detailstr", 1.0F},
                    {"S_diffuse_detail_tiling", 3.0F},
                    {"S_diffuse_detail_tiling_v", 3.0F},
                    {"U_diffdetailoffsetu", 0.0F},
                    {"U_diffdetailoffsetv", 0.0F},
                    {"U_polarscale", 0.5F},
                    {"U_waterlevel", 0.15F},
                    {"S_citymap_tiling", 1.0F},
                    {"S_citymap_tiling_v", 1.0F},
                    {"U_citylevel", 0.15F},
                    {"S_color_glowstr", 1.0F},
                    {"S_color_extraglowstr", 1.0F},
                    {"S_color_extraglow_tiling", 1.0F},
                    {"S_color_extraglow_tiling_v", 1.0F},
                    {"U_extraglowlevel", 0.0F},
                    {"S_normalstr", 1.0F},
                    {"S_normal_detailstr", 1.0F},
                    {"S_normal_detail_tiling", 3.0F},
                    {"S_normal_detail_tiling_v", 3.0F},
                    {"U_cloud_speed", cloudSpeed},
                    {"U_cloud_colorfactor", 0.0F},
                    {"S_cloudmapstr", 1.3F},
                    {"U_cloudsdiffoffsetu", 0.0F},
                    {"S_clouds_detail_maskstr", 1.3F},
                    {"S_clouds_detail_mask_tiling", 3.0F},
                    {"S_clouds_detail_mask_tiling_v", 3.0F},
                    {"U_cloudsdiffdetailoffsetv", 0.0F},
                    {"U_cloudcutoff", 0.2F},
                    {"U_densecutoff", 0.5F},
                    {"U_anisox", 0.0F},
                    {"U_anisoy", 0.0F}},
                   scatteringParams(1025.0F, 1000.0F, 2.0F)),
              gradeParams());
}

const std::vector<ColorParam> kEarthlikeColors{
    {"S_diffuse_color", {0.5F, 0.5F, 0.5F, 1.0F}},
    {"U_color_water", {0.5F, 0.0F, 0.0F, 1.0F}},
    {"S_color_glow_color", {0.0F, 0.0F, 0.0F, 1.0F}},
    {"S_color_extraglow_color", {0.0F, 0.0F, 0.0F, 1.0F}},
    {"U_color_atmo", {0.0F, 0.0F, 0.0F, 1.0F}}};

const std::vector<const char*> kEarthlikeSamplers{
    "diffuse_map",       "diffuse_detail_map", "polar_map",      "color_height_map",
    "color_climate_map", "citymap_map",        "color_glow_map", "color_extraglow_map",
    "normal_map",        "normal_detail_map",  "cloudmap_map",   "clouds_detail_mask_map",
    "densecloudmap_map"};

// planet_earthlike_mv and _mv_un (identical defaults; _mv ignores the lava/water ones).
std::vector<FloatParam> earthlikeMvFloats() {
  return join(join({{"S_diffusestr", 1.0F},
                    {"U_diffoffsetu", 0.0F},
                    {"S_diffuse_detailstr", 1.0F},
                    {"S_diffuse_detail_tiling", 3.0F},
                    {"S_diffuse_detail_tiling_v", 3.0F},
                    {"U_diffdetailoffsetu", 0.0F},
                    {"U_diffdetailoffsetv", 0.0F},
                    {"U_polarscale", 0.5F},
                    {"U_waterlevel", 0.15F},
                    {"S_color_heightstr", 1.0F},
                    {"S_color_climatestr", 1.0F},
                    {"S_citymap_tiling", 1.0F},
                    {"S_citymap_tiling_v", 1.0F},
                    {"U_citylevel", 0.15F},
                    {"S_color_glowstr", 1.0F},
                    {"S_normalstr", 1.0F},
                    {"S_normal_detailstr", 1.0F},
                    {"S_normal_detail_tiling", 3.0F},
                    {"S_normal_detail_tiling_v", 3.0F},
                    {"U_cloud_speed", 0.0012F},
                    {"U_animate_uv0", 1.0F},
                    {"U_cloud_colorfactor", 0.0F},
                    {"U_cloudsdiffoffsetu", 0.0F},
                    {"S_cloudmapstr", 1.0F},
                    {"S_uvoffsetstr", 1.0F},
                    {"U_blur", 0.0F},
                    {"S_moisturestr", 1.0F},
                    {"U_cloudiness", 0.5F},
                    {"U_is_lava", 0.0F},
                    {"U_lavalevel", 0.0F},
                    {"S_water_tiling", 1.0F},
                    {"S_water_tiling_v", 1.0F},
                    {"U_anisox", 0.0F},
                    {"U_anisoy", 0.0F}},
                   scatteringParams(1025.0F, 1000.0F, 2.0F)),
              gradeParams());
}

// planet_gas_mv and _mv_un.
std::vector<FloatParam> gasMvFloats() {
  return join(join({{"S_diffuse_detailstr", 1.0F},
                    {"S_diffuse_detail_tiling", 3.0F},
                    {"S_diffuse_detail_tiling_v", 3.0F},
                    {"U_diffdetailoffsetu", 0.0F},
                    {"U_diffdetailoffsetv", 0.0F},
                    {"U_diffdetailscalev", 1.0F},
                    {"U_diffdetailcutoff", 0.05F},
                    {"U_samples", 1.0F},
                    {"S_uvoffsetstr", 1.0F},
                    {"U_blur", 0.0F},
                    {"U_heightscalev", 1.0F},
                    {"U_heightoffsetu", 0.0F},
                    {"U_heightoffsetv", 0.0F},
                    {"U_colorfactor", 0.0F},
                    {"U_smoothnessmax", 0.0F},
                    {"S_normalstr", 1.0F},
                    {"S_normal_detailstr", 1.0F},
                    {"U_cloudoffsetx", 0.0F},
                    {"U_cloudoffsety", 0.0F},
                    {"U_cloudshadowpower", 1.0F},
                    {"U_cloud_speed", 0.0F},
                    {"U_anisox", 0.0F},
                    {"U_anisoy", 0.0F}},
                   scatteringParams(0.0F, 0.0F, 2.0F)),
              gradeParams());
}

const std::vector<const char*> kGasMvSamplers{"diffuse_map",  "diffuse_detail_map",
                                              "uvoffset_map", "color_height_map",
                                              "normal_map",   "normal_detail_map"};

const std::vector<ColorParam> kEarthlikeMvColors{{"S_diffuse_color", {0.5F, 0.5F, 0.5F, 1.0F}},
                                                 {"U_color_water", {0.5F, 0.0F, 0.0F, 1.0F}},
                                                 {"S_color_glow_color", {0.0F, 0.0F, 0.0F, 1.0F}},
                                                 {"U_color_atmo", {0.0F, 0.0F, 0.0F, 1.0F}}};

const std::vector<const char*> kEarthlikeMvSamplers{
    "diffuse_map",  "diffuse_detail_map", "polar_map",  "color_height_map",  "color_climate_map",
    "citymap_map",  "color_glow_map",     "normal_map", "normal_detail_map", "cloudmap_map",
    "uvoffset_map", "moisture_map",       "water_map"};

// The ring program; flares reuse it with additive blending.
ProgramSpec ringSpec() {
  return {planet_glsl::kRingVs,
          complexUnlitFs(),
          join({{"S_diffusestr", 1.0F},
                {"S_diffuse2str", 1.0F},
                {"S_diffuse3str", 1.0F},
                {"S_alpha1str", 1.0F},
                {"S_alpha2str", 1.0F},
                {"S_alpha3str", 1.0F},
                {"U_dblend1", 1.0F},
                {"U_dblend2", 1.0F},
                {"U_camera_fade_range_near_start", 100.0F},
                {"U_camera_fade_range_near_end", 1000.0F},
                {"U_camera_fade_range_far_start", 100000.0F},
                {"U_camera_fade_range_far_end", 120000.0F},
                {"U_angle_fade_speed", 0.0F},
                {"U_angle_fade_offset", 0.0F},
                {"U_useralphascale", 1.0F}},
               gradeParams()),
          {{"S_diffuse_color", {1.0F, 1.0F, 1.0F, 1.0F}},
           {"U_color_emissive", {1.0F, 1.0F, 1.0F, 1.0F}}},
          {"diffuse_map", "diffuse2_map", "diffuse3_map", "alpha1_map", "alpha2_map", "alpha3_map"},
          true};
}

// Indexed by PlanetPass::Program.
const ProgramSpec& spec(std::size_t program) {
  static const std::array<ProgramSpec, PlanetPass::kProgramCount> kSpecs{{
      {planet_glsl::kBodyVs, bodyFs(planet_glsl::kEarthlikeFs), earthlikeFloats(0.0012F),
       kEarthlikeColors, kEarthlikeSamplers, false},
      {planet_glsl::kBodyVs, bodyFs(planet_glsl::kEarthlikeFs, "#define XENON\n"),
       earthlikeFloats(0.0001F), kEarthlikeColors, kEarthlikeSamplers, false},
      {planet_glsl::kBodyVs, bodyFs(planet_glsl::kEarthlikeMvFs), earthlikeMvFloats(),
       kEarthlikeMvColors, kEarthlikeMvSamplers, false},
      {planet_glsl::kBodyVs, bodyFs(planet_glsl::kEarthlikeMvFs, "#define MV_UN\n"),
       earthlikeMvFloats(), kEarthlikeMvColors, kEarthlikeMvSamplers, false},
      {planet_glsl::kBodyVs,
       bodyFs(planet_glsl::kMoonFs),
       join(join({{"S_diffusestr", 1.0F},
                  {"U_diffoffsetu", 0.0F},
                  {"S_diffuse_detailstr", 1.0F},
                  {"S_diffuse_detail_tiling", 3.0F},
                  {"S_diffuse_detail_tiling_v", 3.0F},
                  {"U_diffdetailoffsetu", 0.0F},
                  {"U_diffdetailoffsetv", 0.0F},
                  {"U_polarscale", 0.5F},
                  {"U_icecolscale", 0.0F},
                  {"U_waterlevel", 0.15F},
                  {"S_citymap_tiling", 1.0F},
                  {"S_citymap_tiling_v", 1.0F},
                  {"U_citylevel", 0.15F},
                  {"S_color_glowstr", 1.0F},
                  {"S_normalstr", 1.0F},
                  {"S_normal_detailstr", 1.0F},
                  {"S_normal_detail_tiling", 3.0F},
                  {"S_normal_detail_tiling_v", 3.0F},
                  {"U_cloud_speed", 0.0012F},
                  {"S_clouds_maskstr", 1.0F},
                  {"U_cloudsdiffoffsetu", 0.0F},
                  {"S_clouds_detail_maskstr", 1.3F},
                  {"S_clouds_detail_mask_tiling", 3.0F},
                  {"S_clouds_detail_mask_tiling_v", 3.0F},
                  {"U_cloudsdiffdetailoffsetu", 0.0F},
                  {"U_cloudsdiffdetailoffsetv", 0.0F},
                  {"U_cloudcutoff", 0.2F},
                  {"U_densecutoff", 0.5F},
                  {"U_anisox", 0.0F},
                  {"U_anisoy", 0.0F}},
                 scatteringParams(0.0F, 0.0F, 2.0F)),
            gradeParams()),
       {{"S_diffuse_color", {0.5F, 0.5F, 0.5F, 1.0F}},
        {"U_color_plains", {0.5F, 0.0F, 0.0F, 1.0F}},
        {"S_color_glow_color", {0.0F, 0.0F, 0.0F, 1.0F}},
        {"U_color_atmo", {0.0F, 0.0F, 0.0F, 1.0F}}},
       {"diffuse_map", "diffuse_detail_map", "polar_map", "citymap_map", "color_glow_map",
        "normal_map", "normal_detail_map", "clouds_mask_map", "clouds_detail_mask_map",
        "cloudmap_map", "densecloudmap_map"},
       false},
      {planet_glsl::kBodyVs,
       bodyFs(planet_glsl::kGasFs),
       join(join({{"S_diffusestr", 1.0F},
                  {"U_diffoffsetu", 0.0F},
                  {"U_diffoffsetv", 0.0F},
                  {"U_diffscalev", 1.0F},
                  {"U_diffspeed", 0.05F},
                  {"S_diffuse_detailstr", 1.0F},
                  {"S_diffuse_detail_tiling", 3.0F},
                  {"S_diffuse_detail_tiling_v", 3.0F},
                  {"U_diffdetailoffsetu", 0.0F},
                  {"U_diffdetailoffsetv", 0.0F},
                  {"U_diffdetailscalev", 1.0F},
                  {"U_diffdetailspeed", 0.05F},
                  {"U_diffdetailcutoff", 0.05F},
                  {"U_samples", 1.0F},
                  {"U_heightscalev", 1.0F},
                  {"U_heightoffsetu", 0.0F},
                  {"U_heightoffsetv", 0.0F},
                  {"U_colorfactor", 0.0F},
                  {"U_smoothnessmax", 0.0F},
                  {"S_normalstr", 1.0F},
                  {"S_normal_detailstr", 1.0F},
                  {"U_cloud_speed", 0.0F},
                  {"U_anisox", 0.0F},
                  {"U_anisoy", 0.0F}},
                 scatteringParams(0.0F, 0.0F, 2.0F)),
            gradeParams()),
       {{"S_diffuse_color", {0.0F, 0.0F, 0.0F, 1.0F}}},
       {"diffuse_map", "diffuse_detail_map", "color_height_map", "normal_map", "normal_detail_map"},
       false},
      {planet_glsl::kBodyVs,
       bodyFs(planet_glsl::kGasMvFs),
       gasMvFloats(),
       {},
       kGasMvSamplers,
       false},
      {planet_glsl::kBodyVs,
       bodyFs(planet_glsl::kGasMvFs, "#define MV_UN\n"),
       gasMvFloats(),
       {},
       kGasMvSamplers,
       false},
      {planet_glsl::kBodyVs,
       bodyFs(planet_glsl::kSunFs),
       join(join({{"S_diffusestr", 1.0F},
                  {"U_diffoffsetu", 0.0F},
                  {"U_diffoffsetv", 0.0F},
                  {"U_diffscalev", 1.0F},
                  {"U_diffspeed", 0.05F},
                  {"U_basesamples", 3.0F},
                  {"S_diffuse_detailstr", 1.0F},
                  {"S_diffuse_detail_tiling", 3.0F},
                  {"S_diffuse_detail_tiling_v", 3.0F},
                  {"U_diffdetailoffsetu", 0.0F},
                  {"U_diffdetailoffsetv", 0.0F},
                  {"U_diffdetailscalev", 1.0F},
                  {"U_diffdetailspeed", 0.05F},
                  {"U_diffdetailcutoff", 0.05F},
                  {"U_samples", 2.0F},
                  {"S_color_heightstr", 1.0F},
                  {"U_heightscalev", 1.0F},
                  {"U_heightoffsetu", 0.0F},
                  {"U_heightoffsetv", 0.0F},
                  {"U_pulsestrength", 0.1F},
                  {"S_normalstr", 1.0F},
                  {"U_cloud_speed", 0.0F}},
                 scatteringParams(0.0F, 0.0F, 2.0F)),
            gradeParams()),
       {},
       {"diffuse_map", "diffuse_detail_map", "color_height_map", "normal_map"},
       false},
      {planet_glsl::kAtmosphereVs,
       planet_glsl::kAtmosphereFs,
       join(scatteringParams(1025.0F, 1000.0F, 4.0F), {{"U_g", -0.48F}}),
       {{"U_color_atmo", {1.0F, 1.0F, 1.0F, 1.0F}}},
       {},
       true},
      ringSpec(),
      {planet_glsl::kAuroraVs,
       complexUnlitFs(),
       join({{"S_diffusestr", 1.0F},
             {"S_diffuse2str", 1.0F},
             {"S_diffuse3str", 1.0F},
             {"U_dblend1", 1.0F},
             {"U_dblend2", 1.0F},
             {"U_camera_fade_near_start", 100.0F},
             {"U_camera_fade_near_stop", 1000.0F},
             {"U_camera_fade_far_start", 100000.0F},
             {"U_camera_fade_far_stop", 120000.0F},
             {"U_angle_fade", 0.0F},
             {"U_useralphascale", 1.0F}},
            gradeParams()),
       {{"S_diffuse_color", {0.5F, 0.5F, 0.5F, 1.0F}},
        {"U_color_emissive", {1.0F, 1.0F, 1.0F, 1.0F}}},
       {"diffuse_map", "diffuse2_map", "diffuse3_map"},
       true},
      ringSpec(),
  }};
  return kSpecs.at(program);
}

struct Entry {
  std::string kind;
  std::string xmf;
  std::string material;
  int body{-1};
  ::Vector3 pos{};
  ::Quaternion quat{};
  float radius{0.0F};
  UvAnimation uvAnimation;
  bool valid{false};
};

// Hostile manifest: any malformed or out-of-range field invalidates the entry.
Entry parseEntry(const nlohmann::json& p) {
  Entry e;
  try {
    e.kind = p.at("kind").get<std::string>();
    e.xmf = p.at("xmf").get<std::string>();
    e.material = p.at("material").get<std::string>();
    e.body = p.at("body").get<int>();
    std::array<double, 3> pos{};
    for (std::size_t i = 0; i < pos.size(); ++i) pos[i] = p.at("pos").at(i).get<double>();
    std::array<double, 4> q{};
    for (std::size_t i = 0; i < q.size(); ++i) q[i] = p.at("quat").at(i).get<double>();
    const double radius = p.at("radius").get<double>();
    const double qlen = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    const double dist = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
    if (!(radius > 0.0 && radius < 1e10) || !(dist < 1e11) || !(qlen > 1e-6 && qlen < 1e6))
      return e;
    e.pos = {static_cast<float>(pos[0]), static_cast<float>(pos[1]), static_cast<float>(pos[2])};
    e.quat = {static_cast<float>(q[0] / qlen), static_cast<float>(q[1] / qlen),
              static_cast<float>(q[2] / qlen), static_cast<float>(q[3] / qlen)};
    e.radius = static_cast<float>(radius);
    if (p.contains("uvAnimation")) e.uvAnimation = uvAnimationFromJson(p.at("uvAnimation"));
    e.valid = true;
  } catch (const nlohmann::json::exception&) {
    e.valid = false;
  }
  return e;
}

// "S_diffusestr" / "U_waterlevel" -> the material property "diffusestr" / "waterlevel".
std::string propertyKey(const char* uniform) {
  return std::string(std::string_view(uniform).substr(2));
}

}  // namespace

// Not ported, so skipped (and counted): p1_planet_bd and p1_planet_earthlike_bor_mv,
// which no cluster references.
std::optional<PlanetPass::Program> PlanetPass::programFor(const std::string& kind,
                                                          const std::string& shader) {
  if (kind == "planet" || kind == "moon") {
    static const std::map<std::string, Program, std::less<>> kBodies{
        {"p1_planet_earthlike", Program::Earthlike},
        {"p1_planet_xenon", Program::Xenon},
        {"p1_planet_earthlike_mv", Program::EarthlikeMv},
        {"p1_planet_earthlike_mv_un", Program::EarthlikeMvUn},
        {"p1_planet_moon", Program::Moon},
        {"p1_planet_gas", Program::Gas},
        {"p1_planet_gas_mv", Program::GasMv},
        {"p1_planet_gas_mv_un", Program::GasMvUn}};
    const auto it = kBodies.find(shader);
    return it != kBodies.end() ? std::optional(it->second) : std::nullopt;
  }
  if (kind == "sun" && shader == "p1_planet_sun") return Program::Sun;
  if (kind == "atmosphere" && shader == "p1_planet_atmosphere_scattering")
    return Program::Atmosphere;
  if (kind == "ring" && shader == "p1_complex_unlit") return Program::Ring;
  if (kind == "aurora" && shader == "p1_aurora") return Program::Aurora;
  if (kind == "flare" && shader == "p1_complex_unlit") return Program::Flare;
  return std::nullopt;
}

int PlanetPass::drawLayer(Program program) {
  switch (program) {
    case Program::Atmosphere:
      return 1;
    case Program::Ring:
      return 2;
    case Program::Aurora:
    case Program::Flare:
      return 3;
    default:
      return 0;
  }
}

PlanetPass::PlanetPass() {
  for (std::size_t i = 0; i < programs_.size(); ++i) {
    Gpu& g = programs_[i];
    const ProgramSpec& s = spec(i);
    g.shader = LoadShaderFromMemory(s.vs, s.fs.c_str());
    g.ok = g.shader.id != 0 && g.shader.id != rlGetShaderIdDefault();
    if (!g.ok) {
      TraceLog(LOG_WARNING, "Planets: shader %zu failed to compile; its parts will not draw", i);
      continue;
    }
    g.mvp = GetShaderLocation(g.shader, "mvp");
    g.worldRot = GetShaderLocation(g.shader, "worldRot");
    g.worldPos = GetShaderLocation(g.shader, "worldPos");
    g.sunDir = GetShaderLocation(g.shader, "V_direction1");
    g.lightColor = GetShaderLocation(g.shader, "V_lightcolor1");
    g.ambient = GetShaderLocation(g.shader, "V_ambient1");
    g.globalLightScale = GetShaderLocation(g.shader, "F_globallightscale");
    g.time = GetShaderLocation(g.shader, "F_time");
    g.uvLinear = GetShaderLocation(g.shader, "uvLinear");
    g.uvOffset = GetShaderLocation(g.shader, "uvOffset");
    g.animateUv0 = GetShaderLocation(g.shader, "U_animate_uv0");
    g.alphaScale = GetShaderLocation(g.shader, "F_alphascale");
    g.vertexData = GetShaderLocation(g.shader, "B_vertexdata0");
  }
  // Unbound samplers read black-transparent: X4's fallback for an absent map is
  // not known, and every Stage-1 material authors the maps its shader reads.
  const std::array<unsigned char, 4> black{0, 0, 0, 0};
  blankTexture_ = rlLoadTexture(black.data(), 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
}

PlanetPass::~PlanetPass() {
  unload();
  for (Gpu& g : programs_)
    if (g.ok) UnloadShader(g.shader);
  if (blankTexture_ != 0) rlUnloadTexture(blankTexture_);
}

void PlanetPass::unload() {
  for (Part& p : parts_) UnloadModel(p.model);
  parts_.clear();
  for (auto& [key, tex] : textures_)
    if (tex.id != 0) UnloadTexture(tex);
  textures_.clear();
  missingAssets_ = 0;
}

unsigned int PlanetPass::texture(const NativeAssets& native, const std::string& address,
                                 bool srgb) {
  if (address.empty()) return 0;
  const auto key = std::make_pair(address, srgb);
  if (const auto it = textures_.find(key); it != textures_.end()) return it->second.id;
  const auto bytes = readTextureDds(archiveAssetReader(*native.archive), address);
  const ::Texture2D tex = bytes ? loadDdsFromMemory(*bytes, srgb) : ::Texture2D{};
  if (tex.id == 0) {
    ++missingAssets_;
    TraceLog(LOG_WARNING, "Planets: failed to load texture %s", address.c_str());
  }
  textures_.emplace(key, tex);  // failures too, so each is reported once
  return tex.id;
}

void PlanetPass::load(const NativeAssets& native, const nlohmann::json& planets) {
  unload();
  if (!planets.is_array()) return;
  const double start = GetTime();

  std::vector<Entry> entries;
  entries.reserve(planets.size());
  for (const auto& p : planets) entries.push_back(parseEntry(p));

  std::vector<std::optional<Program>> programs(entries.size());
  std::map<std::string, int> skipped;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!entries[i].valid) {
      ++skipped["(malformed)"];
      continue;
    }
    const auto def = native.materials.getMaterial(entries[i].material);
    const std::string shader = def ? def->shader : std::string("(no material)");
    programs[i] = programFor(entries[i].kind, shader);
    if (!programs[i]) ++skipped[shader];
  }
  // An atmosphere or ring around a body that is not drawn would float over an
  // empty hole in the sky (the shell's far side shows through), so it goes too.
  // Decided as bodies actually load (a missing mesh drops one too); the
  // manifest lists bodies before their attachments.
  std::vector<bool> drawn(entries.size(), false);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!programs[i]) continue;
    const Entry& e = entries[i];
    if (e.body >= 0 &&
        (static_cast<std::size_t>(e.body) >= i || !drawn[static_cast<std::size_t>(e.body)])) {
      ++skipped["(body skipped)"];
      continue;
    }
    const std::size_t programIndex = static_cast<std::size_t>(*programs[i]);
    const Gpu& g = programs_[programIndex];
    if (!g.ok) continue;
    const ProgramSpec& s = spec(programIndex);

    const auto bytes = archiveAssetReader(*native.archive)(e.xmf);
    auto source = bytes ? parseXmf(*bytes) : std::nullopt;
    // The xmf reader's 1 - 2b/255 decode (verified on hulls) points every planet
    // normal inward; X4's own SIGNED() decode, 2b/255 - 1, is the outward one.
    // Flipping N alone negates cross(N, T), so the bitangent sign flips with it.
    if (source) {
      for (Vec3& n : source->normals) n = {-n.x, -n.y, -n.z};
      for (float& sign : source->bitangentSigns) sign = -sign;
    }
    auto model = source ? modelFromXmf(*source, true, true) : std::nullopt;
    if (!model) {
      ++missingAssets_;
      TraceLog(LOG_WARNING, "Planets: failed to load mesh %s", e.xmf.c_str());
      continue;
    }
    Part part;
    part.model = *model;
    part.program = *programs[i];
    part.pos = e.pos;
    part.rotation = QuaternionToMatrix(e.quat);
    part.radius = e.radius;
    part.uvAnimation = e.uvAnimation;
    part.vertexColors = !source->colors.empty();

    static const MaterialProperties kUnauthored;
    const auto def = native.materials.getMaterial(e.material);
    part.hazeBlend = def && def->blendMode == "HAZEBLEND";
    const MaterialProperties& props = def ? def->properties : kUnauthored;
    for (const FloatParam& f : s.floats) {
      const int loc = GetShaderLocation(g.shader, f.uniform);
      if (loc < 0) continue;
      const auto it = props.floats.find(propertyKey(f.uniform));
      part.floats.emplace_back(loc, it != props.floats.end() ? it->second : f.fallback);
    }
    for (const ColorParam& c : s.colors) {
      const int loc = GetShaderLocation(g.shader, c.uniform);
      if (loc < 0) continue;
      std::array<float, 4> v = c.fallback;
      if (const auto it = props.colors.find(propertyKey(c.uniform)); it != props.colors.end()) {
        for (std::size_t k = 0; k < v.size(); ++k) v[k] = it->second[k] / 255.0F;
        if (s.srgb)
          for (std::size_t k = 0; k < 3; ++k) v[k] = std::pow(v[k], 2.2F);
      }
      part.colors.emplace_back(loc, ::Vector4{v[0], v[1], v[2], v[3]});
    }
    for (const char* name : s.samplers) {
      const std::string sampler = name;
      const std::string stem = sampler.substr(0, sampler.size() - 4);  // drop "_map"
      const auto it = props.bitmaps.find(sampler);
      // complex_unlit's alpha masks are data (its layer defs: "sampled from RGB", LIN).
      const bool srgb = s.srgb && sampler.rfind("alpha", 0) != 0;
      const unsigned int id = it != props.bitmaps.end() ? texture(native, it->second, srgb) : 0U;
      part.bools.emplace_back(GetShaderLocation(g.shader, ("S_" + stem + "_bool").c_str()),
                              id != 0 ? 1 : 0);
      const int loc = GetShaderLocation(g.shader, ("S_" + sampler).c_str());
      if (loc >= 0) part.textures.push_back({loc, id != 0 ? id : blankTexture_});
    }
    parts_.push_back(std::move(part));
    drawn[i] = true;
  }

  // Draw order by layer; the alpha-blended rings back to front. The camera is
  // fixed at the sector origin, so sorting once here holds for every frame.
  // Rings of one planet share a centre; X4 draws the outer one first
  // (ringedplanet.rdc, cluster_416).
  const auto distance = [](const Part& p) { return Vector3Length(p.pos); };
  std::stable_sort(parts_.begin(), parts_.end(), [&](const Part& a, const Part& b) {
    if (a.program == Program::Ring && b.program == Program::Ring) {
      if (distance(a) != distance(b)) return distance(a) > distance(b);
      return a.radius > b.radius;
    }
    return drawLayer(a.program) < drawLayer(b.program);
  });

  // Near plane: under half the closest clearance to any part (low-orbit sectors
  // sit ~2e6 m above the atmosphere), capped at 1e6 m for depth precision. A
  // sector can sit inside a ring's radius, so a ring's clearance is its plane's
  // distance: cluster_416's sector 2 is ~7e5 m off its outer ring's plane.
  double clearance = std::numeric_limits<double>::max();
  double reach = 0.0;
  for (const Part& p : parts_) {
    const double d = Vector3Length(p.pos);
    const double r = p.radius;
    if (p.program == Program::Ring) {
      const ::Vector3 normal = Vector3Transform(ringAxis(p.model), p.rotation);
      clearance =
          std::min(clearance, std::abs(static_cast<double>(Vector3DotProduct(p.pos, normal))));
    } else {
      clearance = std::min(clearance, d - r);
    }
    reach = std::max(reach, d + r);
  }
  near_ = static_cast<float>(std::clamp(0.5 * clearance, 1.0e3, 1.0e6));
  far_ = static_cast<float>(std::max(reach * 1.1, static_cast<double>(near_) * 10.0));

  std::string skippedText;
  for (const auto& [shader, count] : skipped)
    skippedText += " " + shader + " x" + std::to_string(count);
  TraceLog(LOG_INFO, "Planets: %zu parts in %.0f ms (near %.0f m, far %.3g m); skipped:%s",
           parts_.size(), (GetTime() - start) * 1000.0, static_cast<double>(near_),
           static_cast<double>(far_), skippedText.empty() ? " none" : skippedText.c_str());
}

void PlanetPass::drawPart(const Part& part, const ::Matrix& viewProj, ::Vector3 sunDir,
                          const PlanetLight& light) const {
  const Gpu& g = programs_[static_cast<std::size_t>(part.program)];
  const ::Matrix model =
      MatrixMultiply(part.rotation, MatrixTranslate(part.pos.x, part.pos.y, part.pos.z));
  rlEnableShader(g.shader.id);
  rlSetUniformMatrix(g.mvp, MatrixMultiply(model, viewProj));
  rlSetUniformMatrix(g.worldRot, part.rotation);
  rlSetUniform(g.worldPos, &part.pos, RL_SHADER_UNIFORM_VEC3, 1);
  rlSetUniform(g.sunDir, &sunDir, RL_SHADER_UNIFORM_VEC3, 1);
  rlSetUniform(g.lightColor, &light.color, RL_SHADER_UNIFORM_VEC3, 1);
  rlSetUniform(g.ambient, &light.ambient, RL_SHADER_UNIFORM_VEC3, 1);
  rlSetUniform(g.globalLightScale, &kGlobalLightScale, RL_SHADER_UNIFORM_FLOAT, 1);
  const auto time = static_cast<float>(time_);
  rlSetUniform(g.time, &time, RL_SHADER_UNIFORM_FLOAT, 1);
  const UvTransforms uv = evalUvAnimation(part.uvAnimation, time_);
  rlSetUniform(g.uvLinear, uv.linear.data(), RL_SHADER_UNIFORM_VEC4, kUvSets);
  rlSetUniform(g.uvOffset, uv.offset.data(), RL_SHADER_UNIFORM_VEC2, kUvSets);
  const float animateUv0 = 1.0F;  // planet_moon.vert has no gate; the mv programs' floats override
  rlSetUniform(g.animateUv0, &animateUv0, RL_SHADER_UNIFORM_FLOAT, 1);
  const float alphaScale = 1.0F;
  rlSetUniform(g.alphaScale, &alphaScale, RL_SHADER_UNIFORM_FLOAT, 1);
  const int vertexData = part.vertexColors ? 1 : 0;
  rlSetUniform(g.vertexData, &vertexData, RL_SHADER_UNIFORM_INT, 1);
  for (const auto& [loc, v] : part.floats) rlSetUniform(loc, &v, RL_SHADER_UNIFORM_FLOAT, 1);
  for (const auto& [loc, v] : part.colors) rlSetUniform(loc, &v, RL_SHADER_UNIFORM_VEC4, 1);
  for (const auto& [loc, v] : part.bools) rlSetUniform(loc, &v, RL_SHADER_UNIFORM_INT, 1);
  int unit = 0;
  for (const Texture& t : part.textures) {
    rlActiveTextureSlot(unit);
    rlEnableTexture(t.id);
    rlSetUniform(t.samplerLoc, &unit, RL_SHADER_UNIFORM_SAMPLER2D, 1);
    ++unit;
  }
  for (int i = 0; i < part.model.meshCount; ++i) {
    const ::Mesh& mesh = part.model.meshes[i];
    if (!rlEnableVertexArray(mesh.vaoId)) continue;
    if (mesh.indices != nullptr)
      rlDrawVertexArrayElements(0, mesh.triangleCount * 3, nullptr);
    else
      rlDrawVertexArray(0, mesh.vertexCount);
    rlDisableVertexArray();
  }
  while (unit > 0) {
    rlActiveTextureSlot(--unit);
    rlDisableTexture();
  }
  rlDisableShader();
}

void PlanetPass::draw(const ::Camera3D& camera, ::Vector3 sunDir, const PlanetLight& bodyLight,
                      const PlanetLight& shellLight) const {
  if (parts_.empty()) return;
  ::Matrix proj = rlGetMatrixProjection();
  if (proj.m11 > -0.5F) return;  // orthographic: no sky-distance geometry to show
  // Same frustum shape (fovy, aspect, any off-centre terms), planet-range depth.
  proj.m10 = -(far_ + near_) / (far_ - near_);
  proj.m14 = -2.0F * far_ * near_ / (far_ - near_);
  // Rotation only: planets sit ~1e8 m out, the orbit camera moves ~1e4 m.
  const ::Matrix view = MatrixLookAt(::Vector3{0.0F, 0.0F, 0.0F},
                                     Vector3Subtract(camera.target, camera.position), camera.up);
  const ::Matrix viewProj = MatrixMultiply(view, proj);

  rlDrawRenderBatchActive();  // queued sky quads must land before the depth clear
  rlEnableDepthTest();
  rlEnableDepthMask();
  glClear(GL_DEPTH_BUFFER_BIT);
  rlDisableColorBlend();
  for (const Part& p : parts_)
    if (drawLayer(p.program) == 0) drawPart(p, viewProj, sunDir, bodyLight);
  rlEnableColorBlend();
  rlDisableDepthMask();
  // PLANET_ATMOSPHERE (engine-internal, not in the data) is One/One add, depth
  // test on, write off (argon_prime_planet.rdc).
  rlSetBlendFactors(RL_ONE, RL_ONE, RL_FUNC_ADD);
  rlSetBlendMode(RL_BLEND_CUSTOM);
  for (const Part& p : parts_)
    if (p.program == Program::Atmosphere) drawPart(p, viewProj, sunDir, shellLight);
  rlSetBlendMode(RL_BLEND_ALPHA);  // ALPHA8
  for (const Part& p : parts_)
    if (p.program == Program::Ring) drawPart(p, viewProj, sunDir, shellLight);
  // Auroras and flares blend ADDITIVE (One/One add) or HAZEBLEND, which is
  // SrcColor/One add (auroraplanet.rdc): the aurora adds its own square.
  for (const bool haze : {false, true}) {
    rlSetBlendFactors(haze ? RL_SRC_COLOR : RL_ONE, RL_ONE, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM);
    for (const Part& p : parts_)
      if (drawLayer(p.program) == 3 && p.hazeBlend == haze)
        drawPart(p, viewProj, sunDir, shellLight);
  }
  rlSetBlendMode(RL_BLEND_ALPHA);
  rlEnableDepthMask();
  glClear(GL_DEPTH_BUFFER_BIT);
  // Back to the sky pass's state.
  rlDisableDepthMask();
  rlDisableDepthTest();
}

}  // namespace x4sb::editor

#include "sky.hpp"

#include "dds_texture.hpp"
#include "native_assets.hpp"
#include "planet_shaders.hpp"
#include "profiling.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "texture_mips.hpp"
#include "x4sb/assetpipe/native_background.hpp"
#include "x4sb/platform/platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace x4sb::editor {

namespace {

constexpr float kSkyRadius = 500.0f;  // Within default 1000 far clip

// A cluster light as authored (rgb/255 x intensity), and X4's V_lightcolor1 form,
// which linearises the light as a whole, intensity included.
::Vector3 rawLight(::Color c, float intensity) {
  return ::Vector3{static_cast<float>(c.r) / 255.0f * intensity,
                   static_cast<float>(c.g) / 255.0f * intensity,
                   static_cast<float>(c.b) / 255.0f * intensity};
}
::Vector3 decodedLight(::Vector3 v) {
  return ::Vector3{std::pow(v.x, 2.2f), std::pow(v.y, 2.2f), std::pow(v.z, 2.2f)};
}

constexpr const char* kSkyVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
  fragTexCoord = vertexTexCoord;
  fragColor = vertexColor;
  gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";

// p1_simple_unlit as bound for the lens-flare draws (sun.rdc eids 8533-9905):
// linearised texture x diffuseStr x color_emissive (`mul`, CPU-side) x the
// part-mesh vertex color (LinearFromSRGB, common_vert.glsl GetVertexColor).
// X4 samples flare atlases through an sRGB view (BC1_SRGB, path to profit
// sun.rdc), so the decode is part of the authored look (it sharpens every
// glow's falloff); the exact curve, as pow 2.2 is up to 3x too dark on the faint
// texels a thin corona rim is made of. p1_complex_unlit elements
// (sun_corona_v1) add their base colour grade and its [0, 1000] clamp; the
// grade can push a channel negative, which additive blending would subtract.
constexpr const char* kFlareFsHeader = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec3 mul;
uniform vec4 grade;
out vec4 finalColor;
)";
constexpr const char* kFlareFsMain = R"(
void main() {
  vec3 t = texture(texture0, fragTexCoord).rgb;
  vec3 tex = mix(t / 12.92, pow((t + 0.055) / 1.055, vec3(2.4)), step(0.04045, t));
  vec3 c = tex * pow(fragColor.rgb, vec3(2.2)) * mul;
  c = gradeMatrix(grade.x, grade.y, grade.z, grade.w) * vec4(c, 1.0);
  finalColor = vec4(clamp(c, 0.0, 1000.0), 1.0);
}
)";

// p1/high/star.vert.glsl's camera-facing quad, expanded here from a static
// buffer: rebuilding every star's quad on the CPU each frame cost ~3 ms on a
// full star field (known-issues 1.5). `vertexTexCoord2` is the corner (+-1)
// times the star's range; starScale carries the per-frame FOV terms.
constexpr const char* kStarVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec2 vertexTexCoord2;
in vec4 vertexColor;
uniform mat4 mvp;
uniform vec3 camRight;
uniform vec3 camUp;
uniform float starScale;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
  vec3 p = vertexPosition + (camRight * vertexTexCoord2.x + camUp * vertexTexCoord2.y) * starScale;
  fragTexCoord = vertexTexCoord;
  fragColor = vertexColor;
  gl_Position = mvp * vec4(p, 1.0);
}
)";

// p1/high/star.frag.glsl: atlas texel x pow(instance color, 2.2), then the
// base colour grade with its shipped defaults (brightness 0.7, contrast 1,
// saturation 1, hue 0), which reduces to x0.7. The instance colour rides in
// the vertex colour as sRGB bytes so the byte quantisation lands pre-pow.
constexpr const char* kStarFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
out vec4 finalColor;
void main() {
  // p1_star mipbias -1: one level sharper than the footprint says, so a 1-2 px
  // star keeps its sprite core instead of averaging to a grey blob.
  vec3 tex = pow(texture(texture0, fragTexCoord, -1.0).rgb, vec3(2.2));
  vec3 lc = pow(fragColor.rgb, vec3(2.2));
  finalColor = vec4(tex * lc * 0.7, 1.0);
}
)";

::Vector3 linearRgb(const nlohmann::json& bytes) {
  const auto ch = [&bytes](std::size_t i) {
    const float v = static_cast<float>(std::clamp(bytes.at(i).get<int>(), 0, 255)) / 255.0f;
    return std::pow(v, 2.2f);
  };
  return ::Vector3{ch(0), ch(1), ch(2)};
}

// p1/high/star.vert.glsl: the FOV boost that grows star sprites when zoomed
// in (pixelsize 16, boost 1..6 over fov 86deg..4deg, the shipped p1_star.xml
// defaults, confirmed as the bound material values in the capture).
float starFovBoost(float fovyRad) {
  const auto cot = [](float x) { return std::cos(x) / std::sin(x); };
  const float scaleMin = cot(86.0f * DEG2RAD * 0.5f);
  const float scaleMax = cot(4.0f * DEG2RAD * 0.5f);
  const float raw = std::clamp(cot(fovyRad * 0.5f), scaleMin, scaleMax);
  float t = std::clamp((raw - scaleMin) / (scaleMax - scaleMin), 0.0f, 1.0f);
  t = t * t * (3.0f - 2.0f * t);
  return 1.0f + 5.0f * t;
}

// Hostile manifest: the mesh must be self-consistent before it reaches rlgl.
bool loadFlareMesh(const nlohmann::json& m, Sky::SunSpriteGpu& s) {
  constexpr std::size_t kMaxVerts = 65536;
  if (!m.is_object()) return false;
  s.xy = m.at("xy").get<std::vector<float>>();
  s.uv = m.at("uv").get<std::vector<float>>();
  s.rgb = m.at("rgb").get<std::vector<unsigned char>>();
  s.index = m.at("index").get<std::vector<unsigned int>>();
  const std::size_t n = s.xy.size() / 2;
  if (n == 0 || n > kMaxVerts || s.xy.size() != 2 * n || s.uv.size() != 2 * n ||
      s.rgb.size() != 3 * n || s.index.empty() || s.index.size() % 3 != 0)
    return false;
  for (const float f : s.xy)
    if (!std::isfinite(f) || std::abs(f) > 4.0f) return false;
  for (const float f : s.uv)
    if (!std::isfinite(f) || std::abs(f) > 64.0f) return false;
  for (const unsigned int i : s.index)
    if (i >= n) return false;
  return true;
}

// The part mesh as triangles: unit x/y scaled by the full-size basis vectors
// (`right`/`up` are half extents, the mesh spans -0.5..0.5).
void emitMesh(const ::Vector3& pos, const ::Vector3& right, const ::Vector3& up,
              const Sky::SunSpriteGpu& s) {
  // rlgl resets the draw call's texture to the white default both on a batch
  // flush and when rlBegin switches mode (QUADS -> TRIANGLES), so the texture
  // is bound after both.
  rlCheckRenderBatchLimit(static_cast<int>(s.index.size()));
  rlBegin(RL_TRIANGLES);
  rlSetTexture(s.texture.id);
  for (const unsigned int i : s.index) {
    const float x = 2.0f * s.xy[2 * i];
    const float y = 2.0f * s.xy[2 * i + 1];
    const ::Vector3 v = Vector3Add(pos, Vector3Add(Vector3Scale(right, x), Vector3Scale(up, y)));
    rlColor4ub(s.rgb[3 * i], s.rgb[3 * i + 1], s.rgb[3 * i + 2], 255);
    rlTexCoord2f(s.uv[2 * i], s.uv[2 * i + 1]);
    rlVertex3f(v.x, v.y, v.z);
  }
  rlEnd();
}

void emitQuad(const ::Vector3& pos, const ::Vector3& right, const ::Vector3& up,
              const ::Vector4& uv) {
  const ::Vector3 v1 = Vector3Subtract(Vector3Subtract(pos, right), up);
  const ::Vector3 v2 = Vector3Subtract(Vector3Add(pos, right), up);
  const ::Vector3 v3 = Vector3Add(Vector3Add(pos, right), up);
  const ::Vector3 v4 = Vector3Add(Vector3Subtract(pos, right), up);
  rlTexCoord2f(uv.x, uv.w);
  rlVertex3f(v1.x, v1.y, v1.z);
  rlTexCoord2f(uv.z, uv.w);
  rlVertex3f(v2.x, v2.y, v2.z);
  rlTexCoord2f(uv.z, uv.y);
  rlVertex3f(v3.x, v3.y, v3.z);
  rlTexCoord2f(uv.x, uv.y);
  rlVertex3f(v4.x, v4.y, v4.z);
}

}  // namespace

Sky::Sky(const NativeAssets& native, std::string clusterDir, std::string sectorMacro)
    : native_(native) {
  // GL context is live by contract (constructed after InitWindow).
  ::Image glow = GenImageGradientRadial(128, 128, 0.0f, ::Color{255, 255, 255, 255},
                                        ::Color{255, 255, 255, 0});
  sunGlow_ = LoadTextureFromImage(glow);
  UnloadImage(glow);
  SetTextureFilter(sunGlow_, TEXTURE_FILTER_BILINEAR);

  const std::string flareFs = std::string(kFlareFsHeader) + planet_glsl::kSharedFs + kFlareFsMain;
  flareShader_ = LoadShaderFromMemory(kSkyVs, flareFs.c_str());
  starShader_ = LoadShaderFromMemory(kStarVs, kStarFs);
  locFlareMul_ = GetShaderLocation(flareShader_, "mul");
  locFlareGrade_ = GetShaderLocation(flareShader_, "grade");
  locStarRight_ = GetShaderLocation(starShader_, "camRight");
  locStarUp_ = GetShaderLocation(starShader_, "camUp");
  locStarScale_ = GetShaderLocation(starShader_, "starScale");

  clusterDir_ = std::move(clusterDir);
  sectorMacro_ = std::move(sectorMacro);
  valid_ = load();
  if (!valid_) applyFallback();
  uploadStars();
}

void Sky::uploadStars() {
  if (stars_.empty()) return;
  struct StarVertex {
    std::array<float, 3> pos;
    std::array<float, 2> uv;
    std::array<float, 2> corner;
    std::array<unsigned char, 4> color;
  };
  // Two triangles per star, the corner order emitQuad used.
  constexpr std::array<std::array<float, 2>, 6> kCorners{
      {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}}};
  std::vector<StarVertex> verts;
  verts.reserve(stars_.size() * kCorners.size());
  for (const Star& s : stars_) {
    const ::Vector3 p = Vector3Scale(s.dir, kSkyRadius);
    for (const auto& c : kCorners) {
      // 8-column atlas: u = uv.x * 0.125 + specularintensity (star.frag.glsl).
      const float u = c[0] < 0 ? s.spec : s.spec + 0.125f;
      const float v = c[1] < 0 ? 1.0f : 0.0f;
      verts.push_back({{p.x, p.y, p.z},
                       {u, v},
                       {c[0] * s.range, c[1] * s.range},
                       {s.color.r, s.color.g, s.color.b, 255}});
    }
  }
  starVao_ = rlLoadVertexArray();
  rlEnableVertexArray(starVao_);
  starVbo_ =
      rlLoadVertexBuffer(verts.data(), static_cast<int>(verts.size() * sizeof(StarVertex)), false);
  constexpr int kStride = static_cast<int>(sizeof(StarVertex));
  rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION, 3, RL_FLOAT, false, kStride,
                       static_cast<int>(offsetof(StarVertex, pos)));
  rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION);
  rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD, 2, RL_FLOAT, false, kStride,
                       static_cast<int>(offsetof(StarVertex, uv)));
  rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD);
  rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD2, 2, RL_FLOAT, false, kStride,
                       static_cast<int>(offsetof(StarVertex, corner)));
  rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD2);
  rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR, 4, RL_UNSIGNED_BYTE, true, kStride,
                       static_cast<int>(offsetof(StarVertex, color)));
  rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR);
  rlDisableVertexArray();
  rlDisableVertexBuffer();
  starVertexCount_ = static_cast<int>(verts.size());
}

void Sky::applyFallback() {
  // load() can fail partway through a malformed manifest: drop what it already loaded.
  unloadClusterAssets();
  for (int i = 0; i < 1000; ++i) {
    Star s;
    float x = static_cast<float>(GetRandomValue(-100, 100));
    float y = static_cast<float>(GetRandomValue(-100, 100));
    float z = static_cast<float>(GetRandomValue(-100, 100));
    s.dir = Vector3Normalize(::Vector3{x, y, z});
    unsigned char c = static_cast<unsigned char>(GetRandomValue(100, 255));
    s.color = ::Color{c, c, c, 255};
    s.range = 1.5f;
    s.spec = 0.125f * static_cast<float>(GetRandomValue(0, 7));
    stars_.push_back(s);
  }
  sun_.dir = Vector3Normalize(::Vector3{1, 1, 1});
  sun_.color = ::Color{255, 255, 240, 255};
  sun_.intensity = 0.8f;
}

void Sky::unloadClusterAssets() {
  // Many sprites share one atlas texture (deduped at load): unload each GL texture once.
  std::unordered_set<unsigned int> atlases;
  for (const SunSpriteGpu& s : sunSprites_) {
    if (s.texture.id != 0 && s.texture.id != rlGetTextureIdDefault()) atlases.insert(s.texture.id);
  }
  for (const unsigned int id : atlases) rlUnloadTexture(id);
  if (starAtlas_.id != 0) UnloadTexture(starAtlas_);
  starAtlas_ = ::Texture2D{};
  if (starVao_ != 0) rlUnloadVertexArray(starVao_);
  if (starVbo_ != 0) rlUnloadVertexBuffer(starVbo_);
  starVao_ = 0;
  starVbo_ = 0;
  starVertexCount_ = 0;
  stars_.clear();
  domes_.unload();
  sunSprites_.clear();
  planets_.unload();
  fog_.unload();
}

Sky::~Sky() {
  unloadClusterAssets();
  if (sunGlow_.id != 0) UnloadTexture(sunGlow_);
  for (::Shader* s : {&flareShader_, &starShader_}) {
    if (s->id != 0 && s->id != rlGetShaderIdDefault()) UnloadShader(*s);
  }
}

void Sky::reload(const std::string& clusterDir, const std::string& sectorMacro) {
  unloadClusterAssets();
  sun_ = SceneSun{};
  clusterDir_ = clusterDir;
  sectorMacro_ = sectorMacro;
  valid_ = load();
  if (!valid_) applyFallback();
  uploadStars();
}

bool Sky::load() {
  missingAssets_ = 0;
  const auto cluster = clusterDir_.empty() ? std::string("cluster_11") : clusterDir_;
  const auto metadata = assetpipe::nativeBackgroundManifest(*native_.archive, native_.materials,
                                                            cluster, sectorMacro_);
  if (!metadata) {
    TraceLog(LOG_WARNING, "Sky: no background for %s", cluster.c_str());
    return false;
  }

  const auto loadTexture = [&](const std::string& address, const char* what) {
    ::Texture2D texture{};
    if (!address.empty()) {
      const auto bytes = readTextureDds(archiveAssetReader(*native_.archive), address);
      if (bytes) texture = loadDdsFromMemory(*bytes);
    }
    if (texture.id == 0) {
      ++missingAssets_;
      TraceLog(LOG_WARNING, "Sky: failed to load %s %s", what, address.c_str());
    }
    return texture;
  };

  try {
    nlohmann::json manifest = nlohmann::json::parse(*metadata);
    if (manifest.contains("sun")) {
      auto s = manifest["sun"];
      sun_.dir = ::Vector3{s["dir"][0], s["dir"][1], s["dir"][2]};
      sun_.color = ::Color{s["color"][0], s["color"][1], s["color"][2], s["color"][3]};
      sun_.intensity = s["intensity"];
    }
    if (manifest.contains("ambient")) {
      const auto& a = manifest["ambient"];
      sun_.ambientColor =
          ::Color{a.at("color").at(0), a.at("color").at(1), a.at("color").at(2), 255};
      sun_.ambientIntensity = a.at("intensity");
    }

    // .at() here (hostile input): a missing key / short array throws
    // json::out_of_range into the surrounding catch.
    if (manifest.contains("sun") && manifest["sun"].contains("elements")) {
      // Shared atlases: one texture per file, referenced by many sprites.
      std::unordered_map<std::string, ::Texture2D> atlasByFile;
      for (const auto& e : manifest["sun"].at("elements")) {
        SunSpriteGpu s;
        const std::string texFile = e.at("tex");
        if (!loadFlareMesh(e.at("mesh"), s)) continue;
        s.screenMode = e.at("sizeMode") == "screen";
        s.size = e.at("size");
        s.rotation = e.at("rotation");
        if (!(s.size > 0.0f) || s.size > 4.0f) continue;  // inverted: NaN skipped too
        if (!std::isfinite(s.rotation)) continue;         // NaN/inf roll -> degenerate quad
        s.position = e.value("position", 1.0f);
        if (!std::isfinite(s.position) || std::abs(s.position) > 4.0f) continue;
        s.glow = e.value("glow", false);
        // Authored material diffuseStr (absent in pre-intensity manifests -> 1).
        float intensity = e.value("intensity", 1.0f);
        if (!std::isfinite(intensity) || intensity <= 0.0f || intensity > 100.0f) intensity = 1.0f;
        ::Vector3 emissive{1.0f, 1.0f, 1.0f};
        if (e.contains("emissive") && e["emissive"].is_array() && e["emissive"].size() >= 3)
          emissive = linearRgb(e["emissive"]);
        s.mul = Vector3Scale(emissive, intensity);
        s.screenBlend = e.value("screenBlend", false);
        if (e.contains("grade") && e["grade"].is_array() && e["grade"].size() == 4) {
          const auto& g = e["grade"];
          const ::Vector4 grade{g[0].get<float>(), g[1].get<float>(), g[2].get<float>(),
                                g[3].get<float>()};
          const auto sane = [](float v) { return std::isfinite(v) && std::abs(v) <= 1000.0f; };
          if (sane(grade.x) && sane(grade.y) && sane(grade.z) && sane(grade.w)) s.grade = grade;
        }
        auto it = atlasByFile.find(texFile);
        if (it == atlasByFile.end()) {
          ::Texture2D tex = loadTexture(texFile, "sun atlas");
          if (tex.id != 0) SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
          it = atlasByFile.emplace(texFile, tex).first;
        }
        if (it->second.id == 0) continue;
        s.texture = it->second;
        sunSprites_.push_back(s);
      }
      TraceLog(LOG_INFO, "Sky: loaded %zu sun sprites", sunSprites_.size());
    }

    if (manifest.contains("stars")) {
      for (const auto& s : manifest["stars"]) {
        Star st;
        st.dir = ::Vector3{s["dir"][0], s["dir"][1], s["dir"][2]};
        st.color = ::Color{s["color"][0], s["color"][1], s["color"][2], s["color"][3]};
        // p1_star instance colour = rgb x intensity (pre-pow, see kStarFs).
        float b = s.value("brightness", 1.0f);
        if (!std::isfinite(b) || b < 0.0f) b = 1.0f;
        st.color.r = static_cast<unsigned char>(std::clamp(st.color.r * b, 0.0f, 255.0f));
        st.color.g = static_cast<unsigned char>(std::clamp(st.color.g * b, 0.0f, 255.0f));
        st.color.b = static_cast<unsigned char>(std::clamp(st.color.b * b, 0.0f, 255.0f));
        st.range = s.value("range", 1.0f);
        st.spec = s.value("spec", 0.0f);
        if (!std::isfinite(st.range) || st.range <= 0.0f || st.range > 64.0f) st.range = 1.0f;
        if (!std::isfinite(st.spec) || st.spec < 0.0f || st.spec > 0.875f) st.spec = 0.0f;
        stars_.push_back(st);
      }
      TraceLog(LOG_INFO, "Sky: Loaded %zu stars", stars_.size());
    }
    if (manifest.contains("starAtlas")) {
      starAtlas_ = loadTexture(std::string(manifest.at("starAtlas")), "star atlas");
      if (starAtlas_.id != 0) {
        // An 8 px star sampling a 128 px sprite at mip 0 re-picks one texel per
        // frame and twinkles, so a mip-less uncompressed atlas gets a chain.
        ensureUncompressedMips(starAtlas_);
        if (starAtlas_.mipmaps <= 1) SetTextureFilter(starAtlas_, TEXTURE_FILTER_BILINEAR);
      }
    }

    if (manifest.contains("domes")) domes_.load(native_, manifest.at("domes"));
    if (manifest.contains("planets")) planets_.load(native_, manifest.at("planets"));
    if (manifest.contains("fog")) loadFog(manifest.at("fog"), cluster);
  } catch (const std::exception& e) {
    TraceLog(LOG_WARNING, "Sky: malformed background %s: %s", cluster.c_str(), e.what());
    return false;
  }

  return true;
}

// An exact sphere list extracted from a game capture (tools/fog_capture_spheres.py)
// replaces the generated layout, which only matches X4's statistically.
void Sky::loadFog(const nlohmann::json& manifestFog, const std::string& cluster) {
  assetpipe::FogField field = assetpipe::fogFieldFromJson(manifestFog);
  // No cache dir (LOCALAPPDATA unset) must not fall back to a relative path.
  const auto cacheDir = platform::appCacheDir();
  bool exact = false;
  std::ifstream in;
  if (!cacheDir.empty()) in.open(cacheDir / "fog" / (cluster + ".json"), std::ios::binary);
  if (in) {
    const auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    auto spheres = assetpipe::fogSpheresFromJson(j, field.media);
    if (!spheres.empty()) {
      field.spheres = std::move(spheres);
      exact = true;
    }
  }
  fog_.load(field, native_);
  fog_.setSun(sun_.dir, decodedLight(rawLight(sun_.color, sun_.intensity)));
  TraceLog(LOG_INFO, "Sky: fog %zu media, %zu spheres%s", fog_.mediumCount(), fog_.sphereCount(),
           exact ? " (exact capture list)" : "");
}

void Sky::setCameraMotion(float metresPerSecond, float radiansPerSecond) {
  // starfield.frag.glsl: max(0.85, 1 - sat(speed / 10)) x max(0.85, 1 - sat(rot / 0.1745)).
  const auto dim = [](float v) { return std::max(0.85f, 1.0f - std::clamp(v, 0.0f, 1.0f)); };
  starMotion_ = dim(metresPerSecond / 10.0f) * dim(radiansPerSecond / 0.1745f);
}

void Sky::drawStars(const ::Camera3D& camera) {
  if (starVertexCount_ == 0) return;
  // p1/high/star.vert.glsl: a camera-facing quad whose height is
  // pixelsize(16) x range / 1080 of the screen height, scaled by the FOV
  // boost; in world terms 2 * dist * tan(fovy/2) * that. dist = the sky
  // radius the quads sit at (the ratio is what matters).
  const float fovyRad = camera.fovy * DEG2RAD;
  const float frustumHeight = 2.0f * kSkyRadius * std::tan(fovyRad * 0.5f);
  const float starScale = 0.5f * frustumHeight * (16.0f / 1080.0f) * starFovBoost(fovyRad);
  const ::Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
  const ::Vector3 camRight = Vector3Normalize(Vector3CrossProduct(fwd, camera.up));
  const ::Vector3 camUp = Vector3CrossProduct(camRight, fwd);

  rlSetBlendFactorsSeparate(RL_ONE, RL_ONE, RL_ONE, RL_ONE, RL_FUNC_ADD, RL_FUNC_ADD);
  BeginBlendMode(BLEND_CUSTOM_SEPARATE);
  rlEnableShader(starShader_.id);
  const ::Matrix mvp = MatrixMultiply(
      MatrixMultiply(rlGetMatrixTransform(), rlGetMatrixModelview()), rlGetMatrixProjection());
  rlSetUniformMatrix(starShader_.locs[SHADER_LOC_MATRIX_MVP], mvp);
  SetShaderValue(starShader_, locStarRight_, &camRight, SHADER_UNIFORM_VEC3);
  SetShaderValue(starShader_, locStarUp_, &camUp, SHADER_UNIFORM_VEC3);
  SetShaderValue(starShader_, locStarScale_, &starScale, SHADER_UNIFORM_FLOAT);
  rlActiveTextureSlot(0);
  rlEnableTexture(starAtlas_.id != 0 ? starAtlas_.id : rlGetTextureIdDefault());
  rlEnableVertexArray(starVao_);
  rlDrawVertexArray(0, starVertexCount_);
  rlDisableVertexArray();
  rlDisableTexture();
  rlDisableShader();
  EndBlendMode();
}

void Sky::drawGlowSprites() {
  // The glow=1 lens elements: world-space meshes of authored size at the sun's
  // distance (angular in the manifest), ONE/ONE into the HDR scene before
  // bloom (sun.rdc eids 8533/8542) — the sun's core and its authored halo.
  // path to profit sun.rdc (3793-3831): each faces along the camera->sun ray,
  // rolled to world up, not parallel to the image plane; off-axis that makes
  // the rim larger on the near side. The authored `rotation` is not applied:
  // the four coronas share one M_world and the four rays are pixel-identical.
  const ::Vector3 pos = Vector3Scale(sun_.dir, kSkyRadius);
  const ::Vector3 worldUp =
      std::abs(sun_.dir.y) > 0.999f ? ::Vector3{0.0f, 0.0f, 1.0f} : ::Vector3{0.0f, 1.0f, 0.0f};
  const ::Vector3 axisX = Vector3Normalize(Vector3CrossProduct(sun_.dir, worldUp));
  const ::Vector3 axisY = Vector3CrossProduct(axisX, sun_.dir);

  rlSetBlendFactorsSeparate(RL_ONE, RL_ONE, RL_ONE, RL_ONE, RL_FUNC_ADD, RL_FUNC_ADD);
  BeginBlendMode(BLEND_CUSTOM_SEPARATE);
  BeginShaderMode(flareShader_);
  for (const SunSpriteGpu& s : sunSprites_) {
    if (!s.glow || s.screenMode) continue;
    const float half = std::tan(std::min(0.5f * s.size, 1.4f)) * kSkyRadius;
    // raylib defers custom uniforms to batch-flush time, so flush each sprite
    // (below) before the next SetShaderValue.
    SetShaderValue(flareShader_, locFlareMul_, &s.mul, SHADER_UNIFORM_VEC3);
    SetShaderValue(flareShader_, locFlareGrade_, &s.grade, SHADER_UNIFORM_VEC4);
    emitMesh(pos, Vector3Scale(axisX, half), Vector3Scale(axisY, half), s);
    rlDrawRenderBatchActive();
  }
  EndShaderMode();
  EndBlendMode();
  rlSetTexture(rlGetTextureIdDefault());
}

void Sky::draw(const ::Camera3D& camera) {
  ZoneScoped;
  rlDisableDepthMask();
  rlDisableDepthTest();

  rlPushMatrix();
  // Translate to camera position so skybox follows the camera (appears infinitely far)
  rlTranslatef(camera.position.x, camera.position.y, camera.position.z);

  rlDisableBackfaceCulling();  // the camera sits INSIDE the spheres
  domes_.draw(kSkyRadius, starMotion_);

  drawStars(camera);

  if (!sunSprites_.empty() && Vector3Length(sun_.dir) > 0) {
    if (glowEnabled_) drawGlowSprites();
  } else if (Vector3Length(sun_.dir) > 0) {
    // No lens-effect manifest: a procedural glow disc so the sun is at least located.
    ::Vector3 pos = Vector3Scale(sun_.dir, kSkyRadius);
    ::Vector3 fwd = Vector3Normalize(Vector3Negate(pos));
    ::Vector3 upDir = {0, 1, 0};
    if (std::abs(fwd.y) > 0.99f) upDir = {0, 0, 1};
    ::Vector3 right = Vector3Normalize(Vector3CrossProduct(upDir, fwd));
    ::Vector3 up = Vector3CrossProduct(fwd, right);
    const float sunSize = 0.10f * kSkyRadius;
    right = Vector3Scale(right, sunSize);
    up = Vector3Scale(up, sunSize);
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE, RL_ONE, RL_ONE, RL_FUNC_ADD, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
    rlSetTexture(sunGlow_.id != 0 ? sunGlow_.id : rlGetTextureIdDefault());
    rlBegin(RL_QUADS);
    const auto ch = [this](unsigned char c) {
      return static_cast<unsigned char>(
          std::clamp(static_cast<int>(static_cast<float>(c) * sun_.intensity), 0, 255));
    };
    rlColor4ub(ch(sun_.color.r), ch(sun_.color.g), ch(sun_.color.b), 255);
    emitQuad(pos, right, up, ::Vector4{0.0f, 0.0f, 1.0f, 1.0f});
    rlEnd();
    rlSetTexture(rlGetTextureIdDefault());
    EndBlendMode();
  }

  // After the stars and the sun glow: opaque bodies cover both, as in game.
  // argon_prime_planet.rdc: planet BODIES get the lights raw (rgb/255 x
  // intensity); every other draw, the atmosphere shells included, gets them
  // 2.2-decoded.
  const PlanetLight bodyLight{rawLight(sun_.color, sun_.intensity),
                              rawLight(sun_.ambientColor, sun_.ambientIntensity)};
  const PlanetLight shellLight{decodedLight(bodyLight.color), decodedLight(bodyLight.ambient)};
  planets_.draw(camera, sun_.dir, bodyLight, shellLight);

  rlPopMatrix();
  rlEnableBackfaceCulling();  // restore raylib's default state

  rlEnableDepthTest();
  rlEnableDepthMask();
}

void Sky::drawLensFlares(const ::Camera3D& camera, int width, int height) {
  if (sunSprites_.empty() || Vector3Length(sun_.dir) <= 0.0f || width <= 0 || height <= 0) return;
  // Sun in NDC through the scene camera. X4 (sun.rdc eids 9805-9905) draws
  // each non-glow element as its unit part mesh (x +-0.5) scaled by
  // maxsize/scale at z=100 through the main projection, centred at
  // position x sun NDC: quad width = size * H / (2 tan(fovy/2)) pixels, height
  // follows the mesh aspect, ghosts march through the screen centre.
  const ::Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
  const ::Vector3 camRight = Vector3Normalize(Vector3CrossProduct(fwd, camera.up));
  const ::Vector3 camUp = Vector3CrossProduct(camRight, fwd);
  const float depth = Vector3DotProduct(sun_.dir, fwd);
  if (depth <= 1e-4f) return;  // behind the camera: X4's fade-out endpoint
  const float fovyRad = camera.fovy * DEG2RAD;
  const float tanHalf = std::tan(0.5f * fovyRad);
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  const float ndcX = Vector3DotProduct(sun_.dir, camRight) / (depth * tanHalf * aspect);
  const float ndcY = Vector3DotProduct(sun_.dir, camUp) / (depth * tanHalf);
  // X4 draws this half only while the sun itself is inside the frame: station.rdc
  // (sun at NDC -1.4,-6.7) has zero screen-space lens draws, and sun_c_14's
  // elements carry fadeangle=0 / inviewduration=0, i.e. a hard cut, no fade.
  if (std::abs(ndcX) > 1.0f || std::abs(ndcY) > 1.0f) return;
  const float cx = 0.5f * static_cast<float>(width);
  const float cy = 0.5f * static_cast<float>(height);

  BeginShaderMode(flareShader_);
  for (const SunSpriteGpu& s : sunSprites_) {
    if (s.glow || !s.screenMode) continue;
    const float px = cx + ndcX * s.position * cx;
    const float py = cy - ndcY * s.position * cy;
    const float w = s.size * static_cast<float>(height) / (2.0f * tanHalf);
    const float cr = std::cos(s.rotation);
    const float sr = std::sin(s.rotation);
    const ::Vector3 right{0.5f * w * cr, 0.5f * w * sr, 0.0f};
    const ::Vector3 up{-0.5f * w * sr, 0.5f * w * cr, 0.0f};
    if (s.screenBlend) {
      rlSetBlendFactorsSeparate(RL_ONE, RL_ONE_MINUS_SRC_COLOR, RL_ONE, RL_ZERO, RL_FUNC_ADD,
                                RL_FUNC_ADD);
    } else {
      rlSetBlendFactorsSeparate(RL_ONE, RL_ONE, RL_ONE, RL_ZERO, RL_FUNC_ADD, RL_FUNC_ADD);
    }
    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
    SetShaderValue(flareShader_, locFlareMul_, &s.mul, SHADER_UNIFORM_VEC3);
    SetShaderValue(flareShader_, locFlareGrade_, &s.grade, SHADER_UNIFORM_VEC4);
    // 2D context: y grows downward, so the mesh's +y maps to screen-up negated.
    emitMesh(::Vector3{px, py, 0.0f}, right, Vector3Negate(up), s);
    rlDrawRenderBatchActive();  // bake mul + blend state before the next sprite
    EndBlendMode();
  }
  EndShaderMode();
  rlSetTexture(rlGetTextureIdDefault());
}

}  // namespace x4sb::editor

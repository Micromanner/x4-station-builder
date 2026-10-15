#include "sky_domes.hpp"

#include "dds_texture.hpp"
#include "external/glad.h"
#include "native_assets.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "x4sb/assetpipe/native_background.hpp"
#include "x4sb/assetpipe/xmf.hpp"
#include "xmf_model.hpp"

#include <cmath>
#include <optional>
#include <string>

namespace x4sb::editor {

namespace {

// high/common.vert.glsl: DATA0 is sRGB-decoded as pow(c + 1e-5, 2.2), alpha raw.
// UV1 is p1_stars' mask coordinate (zero on meshes without one).
constexpr const char* kVs = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec2 vertexTexCoord2;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 uv0;
out vec2 uv1;
out vec4 vcolor;
void main() {
  uv0 = vertexTexCoord;
  uv1 = vertexTexCoord2;
  vcolor = vec4(pow(vertexColor.rgb + vec3(1e-5), vec3(2.2)), vertexColor.a);
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

// Maps are uploaded sRGB (X4 samples them through SRGB_sampler2D views). The sky
// is drawn first onto the cleared target, so alpha is written as 1 throughout.
constexpr const char* kFsHeader = R"(#version 330
in vec2 uv0;
in vec2 uv1;
in vec4 vcolor;
uniform sampler2D texture0;  // diffuse_map
uniform sampler2D texture1;  // diffuse2_map
uniform sampler2D texture2;  // diffuse3_map
uniform bool hasDiffuse;
uniform bool hasDiffuse2;
uniform bool hasDiffuse3;
uniform vec4 color;
uniform vec3 strength;  // diffusestr, diffuse2str, diffuse3str
uniform vec3 tiling;    // diffuse_tiling, diffuse2_tiling, diffuse3_tiling
out vec4 finalColor;
)";

// p1/high/simple_unlit.frag.glsl. No saturate: the dome is the linear vertex colour.
constexpr const char* kSimpleUnlitFs = R"(
void main() {
  vec4 c = color;
  if (hasDiffuse) {
    c *= texture(texture0, uv0);
    c.rgb *= strength.x;
  }
  c.rgb *= vcolor.rgb;
  finalColor = vec4(c.rgb, 1.0);
}
)";

// p1/high/background.frag.glsl (shader p1_stars): a bound map REPLACES the emissive
// colour, the mask rides UV1, and the result saturates. Its colour-grade params are
// declared in p1_stars.xml but never read.
constexpr const char* kStarsFs = R"(
void main() {
  vec4 c = color;
  if (hasDiffuse) c = strength.x * texture(texture0, uv0);
  if (hasDiffuse2) c *= strength.y * texture(texture1, uv1);
  c.rgb *= vcolor.rgb;
  finalColor = vec4(clamp(c.rgb, 0.0, 1.0), 1.0);
}
)";

// Starfield dome layers plus procedural pixel stars anchored to world directions.
constexpr const char* kStarfieldFs = R"(
uniform vec4 layer1Boost;  // boostmin, boostmax, fovmin, fovmax
uniform vec4 layer2Boost;
uniform float projScale;   // |M_projection[1][1]|
uniform mat4 invProjection;
uniform mat4 invView;
uniform vec2 viewport;
uniform float starMotion;  // camera-motion dim

// Two decorrelated star layers; layer 2's grid is the base x1.333, +1.
const int kCells1 = 1080;
const int kCells2 = 1440;
const float kStarRadiusPx = 0.5;
const float kStarEdgePx = 0.35;

float fovBoost(vec4 b) {
  float sLo = 1.0 / tan(radians(b.w) * 0.5);
  float sHi = 1.0 / tan(radians(b.z) * 0.5);
  float w = clamp((clamp(projScale, sLo, sHi) - sLo) / (sHi - sLo), 0.0, 1.0);
  return mix(b.x, b.y, smoothstep(0.0, 1.0, w));
}

// PCG3D (Jarzynski and Olano, JCGT 2020).
uvec3 pcg3d(uvec3 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  return v;
}

float unorm(uint h) { return float(h >> 8u) * (1.0 / 16777216.0); }

vec3 signNonZero(vec3 v) { return step(vec3(0.0), v) * 2.0 - 1.0; }

// Octahedral map to [-1,1]^2 with the fold region pinned to s: with s = the
// direction's own signs this is the standard encoding, and pinning s lets a
// neighbouring direction be mapped linearly across a fold for the Jacobian.
vec2 octEncode(vec3 d, vec3 s) {
  vec2 p = d.xy / dot(d, s);
  return s.z < 0.0 ? (1.0 - p.yx * s.yx) * s.xy : p;
}

// The octahedral square tiles the plane by mirroring across each edge
// ((x, y) -> (2 - x, -y)), so neighbour cells off the square wrap to their real
// sphere neighbours. Each mirror flips both in-cell axes; two cancel.
ivec2 wrapCell(ivec2 c, int n, out bool flipped) {
  flipped = false;
  if (c.x < 0 || c.x >= n) {
    c = ivec2(c.x < 0 ? -1 - c.x : 2 * n - 1 - c.x, n - 1 - c.y);
    flipped = !flipped;
  }
  if (c.y < 0 || c.y >= n) {
    c = ivec2(n - 1 - c.x, c.y < 0 ? -1 - c.y : 2 * n - 1 - c.y);
    flipped = !flipped;
  }
  return c;
}

uvec3 cellHash(ivec2 c, uint seed) { return pcg3d(uvec3(uvec2(c), seed)); }

// A cell keeps its star only if no occupied cell in its 5x5 neighbourhood has a
// lower occupancy hash, which keeps surviving stars at least 3 cells apart.
bool winsNeighbourhood(ivec2 c, int n, uint seed, uint own, float p) {
  for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x) {
      if (x == 0 && y == 0) continue;
      bool unused;
      uint h = cellHash(wrapCell(c + ivec2(x, y), n, unused), seed).x;
      if (h < own && unorm(h) < p) return false;
    }
  return true;
}

float starLayer(vec3 dir, vec3 dirDx, vec3 dirDy, int n, uint seed, float p) {
  vec3 s = signNonZero(dir);
  float halfN = 0.5 * float(n);
  vec2 oct = octEncode(dir, s);
  vec2 cc = (oct * 0.5 + 0.5) * float(n);
  mat2 j = mat2((octEncode(dir + dirDx, s) - oct) * halfN,
                (octEncode(dir + dirDy, s) - oct) * halfN);
  if (abs(determinant(j)) < 1e-12) return 0.0;
  mat2 cellToPx = inverse(j);

  ivec2 base = ivec2(floor(cc));
  float best = 0.0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      ivec2 cell = base + ivec2(x, y);
      bool flipped;
      ivec2 canon = wrapCell(cell, n, flipped);
      uvec3 h = cellHash(canon, seed);
      if (unorm(h.x) >= p) continue;
      vec2 offset = 0.15 + 0.7 * vec2(unorm(h.y), unorm(h.z));
      if (flipped) offset = 1.0 - offset;
      float distPx = length(cellToPx * (vec2(cell) + offset - cc));
      if (distPx >= kStarRadiusPx + 0.5 * kStarEdgePx) continue;
      if (!winsNeighbourhood(canon, n, seed, h.x, p)) continue;
      float u = unorm(pcg3d(h).x);
      float coverage = 1.0 - smoothstep(kStarRadiusPx - 0.5 * kStarEdgePx,
                                        kStarRadiusPx + 0.5 * kStarEdgePx, distPx);
      best = max(best, (0.025 + 0.025 * u * u * u) * coverage);
    }
  return best;
}

// Fixed 0.71 rad turn about an oblique axis decorrelates layer 2 from layer 1.
vec3 rotateLayer2(vec3 v) {
  const vec3 k = vec3(0.4170, -0.6056, 0.6778);
  const float c = 0.7584;
  const float sn = 0.6518;
  return v * c + cross(k, v) * sn + k * dot(k, v) * (1.0 - c);
}

void main() {
  float boost1 = fovBoost(layer1Boost);
  float boost2 = fovBoost(layer2Boost);
  vec4 c = color;
  if (hasDiffuse) {
    c *= texture(texture0, uv0 * tiling.x);
    c.rgb *= strength.x * boost1;
    if (hasDiffuse3) c.rgb *= texture(texture2, uv0 * tiling.z).rgb * strength.z;
  }
  if (hasDiffuse2) {
    float mask = hasDiffuse3 ? texture(texture2, uv0 * tiling.z * 8.0).r * strength.z : 1.0;
    c.rgb += texture(texture1, uv0 * tiling.y).rgb * strength.y * boost2 * vec3(1.0, 1.0, 2.0) * mask;
  }
  c.rgb *= vcolor.rgb;

  float density = 0.001 * (vcolor.b + texture(texture2, uv0 * tiling.z).r);
  vec3 tint = vec3(1.0, 0.95, 0.92) * 0.45 * vcolor.rgb * texture(texture2, uv0 * 0.2).r;

  // Stars hang off the pixel's view direction, not the dome UVs, so they stay
  // fixed to the sky under rotation and zoom. Game axes have z negated.
  vec2 ndc = (floor(gl_FragCoord.xy) + 0.5) / viewport * 2.0 - 1.0;
  vec4 nearPt = invProjection * vec4(ndc, -1.0, 1.0);
  vec3 dir = normalize(mat3(invView) * (nearPt.xyz / nearPt.w)) * vec3(1.0, 1.0, -1.0);
  // Derivatives before any divergent branch.
  vec3 dirDx = dFdx(dir);
  vec3 dirDy = dFdy(dir);

  float p = 25.0 * min(0.5 * density, 0.5);
  float star = starLayer(dir, dirDx, dirDy, kCells1, 0x2f6bu, p);
  star = max(star, starLayer(rotateLayer2(dir), rotateLayer2(dirDx), rotateLayer2(dirDy),
                             kCells2, 0x9e37u, p));
  finalColor = vec4(c.rgb + star * tint * starMotion, 1.0);
}
)";

float floatProperty(const MaterialProperties& props, const std::string& name, float fallback) {
  const auto it = props.floats.find(name);
  return it != props.floats.end() ? it->second : fallback;
}

// X4 sRGB-decodes Color props for shaders that sample through sRGB views.
::Vector4 colorProperty(const MaterialProperties& props, const std::string& name,
                        ::Vector4 fallback) {
  const auto it = props.colors.find(name);
  if (it == props.colors.end()) return fallback;
  const auto& c = it->second;
  const auto decode = [](float byte) { return std::pow(byte / 255.0F, 2.2F); };
  return ::Vector4{decode(c[0]), decode(c[1]), decode(c[2]), c[3] / 255.0F};
}

::Vector4 multiply(::Vector4 a, ::Vector4 b) {
  return ::Vector4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

}  // namespace

// Shader defaults from shadergl/ogl/p1_{simple_unlit,stars,starfield}.xml.
SkyDomes::Layer SkyDomes::layerFor(const MaterialDef& def) {
  Layer layer;
  if (def.shader == "p1_stars") {
    layer.program = Program::Stars;
  } else if (def.shader == "p1_starfield") {
    layer.program = Program::Starfield;
  } else if (def.shader != "p1_simple_unlit") {
    TraceLog(LOG_INFO, "Sky: dome shader %s drawn as p1_simple_unlit", def.shader.c_str());
  }
  if (def.blendMode == "ADDITIVE") layer.blend = Blend::Additive;
  if (def.blendMode == "SCREEN") layer.blend = Blend::Screen;

  const MaterialProperties& props = def.properties;
  const ::Vector4 white{1.0F, 1.0F, 1.0F, 1.0F};
  const ::Vector4 emissive = colorProperty(props, "color_emissive", white);
  if (layer.program == Program::Stars) {
    const ::Vector4 diffuse = colorProperty(props, "diffuse_color", {0.5F, 0.5F, 0.5F, 1.0F});
    layer.color = ::Vector4{emissive.x, emissive.y, emissive.z, diffuse.w};
  } else {
    layer.color = multiply(colorProperty(props, "diffuse_color", white), emissive);
  }
  layer.strength = {floatProperty(props, "diffusestr", 1.0F),
                    floatProperty(props, "diffuse2str", 1.0F),
                    floatProperty(props, "diffuse3str", 1.0F)};
  layer.tiling = {floatProperty(props, "diffuse_tiling", 1.0F),
                  floatProperty(props, "diffuse2_tiling", 1.0F),
                  floatProperty(props, "diffuse3_tiling", 1.0F)};
  const auto boost = [&](const char* prefix, ::Vector4 fallback) {
    const std::string p = prefix;
    return ::Vector4{floatProperty(props, p + "_boostmin", fallback.x),
                     floatProperty(props, p + "_boostmax", fallback.y),
                     floatProperty(props, p + "_fovmin", fallback.z),
                     floatProperty(props, p + "_fovmax", fallback.w)};
  };
  layer.layer1Boost = boost("layer1", layer.layer1Boost);
  layer.layer2Boost = boost("layer2", layer.layer2Boost);
  return layer;
}

void SkyDomes::setBlend(Blend blend) {
  if (blend == Blend::Screen) {
    rlSetBlendFactorsSeparate(RL_ONE, RL_ONE_MINUS_SRC_COLOR, RL_ONE, RL_ZERO, RL_FUNC_ADD,
                              RL_FUNC_ADD);
  } else {
    rlSetBlendFactorsSeparate(RL_ONE, RL_ONE, RL_ONE, RL_ZERO, RL_FUNC_ADD, RL_FUNC_ADD);
  }
  BeginBlendMode(BLEND_CUSTOM_SEPARATE);
}

SkyDomes::SkyDomes() {
  const std::array<const char*, kProgramCount> bodies{kSimpleUnlitFs, kStarsFs, kStarfieldFs};
  for (std::size_t i = 0; i < programs_.size(); ++i) {
    ProgramGpu& p = programs_[i];
    const std::string fs = std::string(kFsHeader) + bodies[i];
    p.shader = LoadShaderFromMemory(kVs, fs.c_str());
    p.hasMapLocs = {GetShaderLocation(p.shader, "hasDiffuse"),
                    GetShaderLocation(p.shader, "hasDiffuse2"),
                    GetShaderLocation(p.shader, "hasDiffuse3")};
    p.color = GetShaderLocation(p.shader, "color");
    p.strength = GetShaderLocation(p.shader, "strength");
    p.tiling = GetShaderLocation(p.shader, "tiling");
    p.layer1Boost = GetShaderLocation(p.shader, "layer1Boost");
    p.layer2Boost = GetShaderLocation(p.shader, "layer2Boost");
    p.projScale = GetShaderLocation(p.shader, "projScale");
    p.invProjection = GetShaderLocation(p.shader, "invProjection");
    p.invView = GetShaderLocation(p.shader, "invView");
    p.viewport = GetShaderLocation(p.shader, "viewport");
    p.starMotion = GetShaderLocation(p.shader, "starMotion");
  }
}

SkyDomes::~SkyDomes() {
  unload();
  for (const ProgramGpu& p : programs_)
    if (p.shader.id != 0 && p.shader.id != rlGetShaderIdDefault()) UnloadShader(p.shader);
}

::Texture2D SkyDomes::texture(const NativeAssets& native, const std::string& address) {
  if (address.empty()) return {};
  if (const auto it = textures_.find(address); it != textures_.end()) return it->second;
  const auto bytes = readTextureDds(archiveAssetReader(*native.archive), address);
  ::Texture2D tex = bytes ? loadDdsFromMemory(*bytes, /*srgb=*/true) : ::Texture2D{};
  if (tex.id == 0) {
    ++missingAssets_;
    TraceLog(LOG_WARNING, "Sky: failed to load dome texture %s", address.c_str());
  } else {
    SetTextureFilter(tex, tex.mipmaps > 1 ? TEXTURE_FILTER_TRILINEAR : TEXTURE_FILTER_BILINEAR);
  }
  textures_.emplace(address, tex);  // failures too, so each is reported once
  return tex;
}

void SkyDomes::load(const NativeAssets& native, const nlohmann::json& domes) {
  constexpr std::array<int, 3> kMapSlots{MATERIAL_MAP_ALBEDO, MATERIAL_MAP_METALNESS,
                                         MATERIAL_MAP_NORMAL};
  constexpr std::array<const char*, 3> kMapNames{"diffuse_map", "diffuse2_map", "diffuse3_map"};
  for (const auto& d : domes) {
    Dome dome;
    dome.pos = ::Vector3{d.at("pos").at(0), d.at("pos").at(1), d.at("pos").at(2)};
    dome.quat =
        ::Vector4{d.at("quat").at(0), d.at("quat").at(1), d.at("quat").at(2), d.at("quat").at(3)};
    dome.extent = d.at("extent");
    if (!(dome.extent > 0.0F)) continue;  // inverted so NaN is skipped too

    // The X4->display (1,1,-1) flip inverts winding; the draw disables culling.
    const std::string xmf = d.at("xmf");
    const std::string partMaterial = d.at("material");
    const auto bytes = archiveAssetReader(*native.archive)(xmf);
    const auto source = bytes ? parseXmf(*bytes) : std::nullopt;
    const auto prepared =
        source ? prepareXmfModel(*source, /*rawColors=*/true, /*tangentFrame=*/false, /*uv1=*/true)
               : std::nullopt;
    auto model = prepared ? uploadXmfModel(*prepared) : std::nullopt;
    if (!model) {
      ++missingAssets_;
      TraceLog(LOG_WARNING, "Sky: failed to load dome %s", xmf.c_str());
      continue;
    }
    dome.model = *model;
    for (int m = 0; m < dome.model.meshCount; ++m) {
      const auto index = static_cast<std::size_t>(m);
      const std::string& reference =
          index < source->submeshes.size() ? source->submeshes[index].materialRef : partMaterial;
      const auto def = native.materials.getMaterial(reference);
      if (!def) {
        // Guessing a shader would paint an opaque sphere over the sky.
        ++missingAssets_;
        TraceLog(LOG_WARNING, "Sky: dome material %s not found", reference.c_str());
        dome.layers.push_back(Layer{.drawn = false});
        continue;
      }
      Layer layer = layerFor(*def);
      ::Material& material = dome.model.materials[dome.model.meshMaterial[m]];
      material.shader = programs_[static_cast<std::size_t>(layer.program)].shader;
      for (std::size_t k = 0; k < kMapSlots.size(); ++k) {
        ::Texture2D tex{};
        const auto it = def->properties.bitmaps.find(kMapNames[k]);
        if (it != def->properties.bitmaps.end()) tex = texture(native, it->second);
        layer.hasMap[k] = tex.id != 0;
        // starfield.frag samples diffuse3 unguarded: an absent map reads white,
        // never whatever texture the unit last held.
        if (tex.id == 0)
          tex = ::Texture2D{rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        material.maps[kMapSlots[k]].texture = tex;
      }
      dome.layers.push_back(layer);
    }
    domes_.push_back(std::move(dome));
  }
  TraceLog(LOG_INFO, "Sky: loaded %zu dome meshes", domes_.size());
}

void SkyDomes::unload() {
  // UnloadModel frees the material maps but not their textures, which are ours.
  for (const Dome& d : domes_) UnloadModel(d.model);
  domes_.clear();
  for (auto& [address, tex] : textures_)
    if (tex.id != 0) UnloadTexture(tex);
  textures_.clear();
  missingAssets_ = 0;
}

void SkyDomes::drawLayer(const Dome& d, int mesh, float skyRadius, float starMotion) const {
  const Layer& layer = d.layers[static_cast<std::size_t>(mesh)];
  const ProgramGpu& p = programs_[static_cast<std::size_t>(layer.program)];
  for (std::size_t k = 0; k < layer.hasMap.size(); ++k) {
    const int on = layer.hasMap[k] ? 1 : 0;
    SetShaderValue(p.shader, p.hasMapLocs[k], &on, SHADER_UNIFORM_INT);
  }
  SetShaderValue(p.shader, p.color, &layer.color, SHADER_UNIFORM_VEC4);
  SetShaderValue(p.shader, p.strength, layer.strength.data(), SHADER_UNIFORM_VEC3);
  SetShaderValue(p.shader, p.tiling, layer.tiling.data(), SHADER_UNIFORM_VEC3);
  SetShaderValue(p.shader, p.layer1Boost, &layer.layer1Boost, SHADER_UNIFORM_VEC4);
  SetShaderValue(p.shader, p.layer2Boost, &layer.layer2Boost, SHADER_UNIFORM_VEC4);
  SetShaderValue(p.shader, p.starMotion, &starMotion, SHADER_UNIFORM_FLOAT);

  // The manifest's dome units are X4 metres (extent ~5e8): scale to the sky radius
  // keeping the authored offset/orientation. Depth is off, so the radius is cosmetic,
  // but the far plane is not: a small sheet far off-centre (Cluster_15 nebula001 sits
  // 8e7 out at extent 1e8) must still land inside it, hence |pos| + extent.
  const float k = skyRadius / (Vector3Length(d.pos) + d.extent);
  rlPushMatrix();
  rlTranslatef(d.pos.x * k, d.pos.y * k, d.pos.z * k);
  const ::Quaternion q{d.quat.x, d.quat.y, d.quat.z, d.quat.w};
  rlMultMatrixf(MatrixToFloatV(QuaternionToMatrix(q)).v);
  rlScalef(k, k, k);
  // DrawMesh composes with the current rlgl matrix stack (verified raylib fact).
  DrawMesh(d.model.meshes[mesh], d.model.materials[d.model.meshMaterial[mesh]], MatrixIdentity());
  rlPopMatrix();
}

void SkyDomes::draw(float skyRadius, float starMotion) const {
  if (domes_.empty()) return;
  // Per-frame starfield inputs: the camera-locked modelview's rotation is the view.
  const ProgramGpu& starfield = programs_[static_cast<std::size_t>(Program::Starfield)];
  const ::Matrix projection = rlGetMatrixProjection();
  const float projScale = std::abs(projection.m5);
  std::array<GLint, 4> viewport{};
  glGetIntegerv(GL_VIEWPORT, viewport.data());
  const ::Vector2 viewportSize{static_cast<float>(viewport[2]), static_cast<float>(viewport[3])};
  SetShaderValue(starfield.shader, starfield.projScale, &projScale, SHADER_UNIFORM_FLOAT);
  SetShaderValue(starfield.shader, starfield.viewport, &viewportSize, SHADER_UNIFORM_VEC2);
  SetShaderValueMatrix(starfield.shader, starfield.invProjection, MatrixInvert(projection));
  SetShaderValueMatrix(starfield.shader, starfield.invView, MatrixInvert(rlGetMatrixModelview()));

  // X4 order (sun.rdc eids 8488 -> 8509 -> 8521): the opaque dome, then the
  // blended star layers in part order.
  rlDrawRenderBatchActive();
  rlDisableColorBlend();
  for (const Dome& d : domes_)
    for (int m = 0; m < d.model.meshCount; ++m)
      if (const Layer& l = d.layers[static_cast<std::size_t>(m)];
          l.drawn && l.blend == Blend::Opaque)
        drawLayer(d, m, skyRadius, starMotion);
  rlEnableColorBlend();
  for (const Dome& d : domes_)
    for (int m = 0; m < d.model.meshCount; ++m) {
      const Layer& l = d.layers[static_cast<std::size_t>(m)];
      if (!l.drawn || l.blend == Blend::Opaque) continue;
      setBlend(l.blend);
      drawLayer(d, m, skyRadius, starMotion);
      EndBlendMode();
    }
}

}  // namespace x4sb::editor

#include "mesh_cache.hpp"

#include "dds_texture.hpp"
#include "env_cubemap.hpp"
#include "external/glad.h"
#include "planet_shaders.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "visibility_buffer.hpp"
#include "x4sb/assetpipe/dds.hpp"
#include "x4sb/assetpipe/native_background.hpp"
#include "xmf_model.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace x4sb::editor {
namespace {
// Fixed texture units of the per-frame resources; the station shaders declare the
// same numbers as layout(binding) (env 9, AO 11, light buffers 12-14).
constexpr int kSceneDepthUnit = 15;
constexpr std::array<int, 5> kShadowUnits{16, 17, 18, 19, 10};
// kStationVsHead's UvAnim reads the evaluated UvTransforms as uploaded (std430).
static_assert(sizeof(UvTransforms) == 96 && offsetof(UvTransforms, offset) == 64);

// The station programs' shared blocks; C++ mirrors and offsets in station_gpu.hpp.
constexpr const char* kFrameDataGlsl = R"(
layout(std140, binding = 0) uniform FrameData {
  mat4 lightVP0, lightVP1, lightVP2, lightVP3, lightVP4;
  mat4 lightView;
  vec3 sunDirection;
  float sunIntensity;
  vec3 sunColor;
  float envMaxLod;
  vec3 ambientLight;  // X4 V_ambient1: the cluster <ambient>, 2.2-decoded
  float animTime;     // X4 F_time, seconds
  vec3 cameraPos;
  int lightMask;  // debug: bit0 sun diffuse, bit1 sun specular, bit2 IBL (env reflection), bit3 local lights; 16 alone = shadow term, 32 albedo, 64 roughness/metal, 128 SSAO, 256 normal
  float shadowTexel0, shadowTexel1, shadowTexel2, shadowTexel3, shadowTexel4;  // map uv
  int shadowCascades;  // bit i = cascade i valid
  int useAo;
  int usePom;          // X4 B_pom_enabled
  vec2 aoScreen;       // scene target size: aoMap is sampled by screen uv
  vec2 clusterViewport;
  vec2 sliceParams;    // slice = log(viewZ) * x + y
  vec2 depthViewport;
  vec2 depthRange;     // near, far clip
  int hasLocalLights;
  int hasSceneDepth;   // 0 = no scene depth copy this frame
  int normalBinormalSign;  // X4 binormal sign after the display-space Z mirror; default -1
};
)";

// The multi-draw's per-draw state (C++ DrawData), at drawBase + gl_DrawIDARB, which the
// vertex stage hands on as the flat vDrawIndex. The aliases keep the shader bodies (and
// the complex_unlit body shared with the planets) reading plain names.
constexpr const char* kDrawDataGlsl = R"(
struct DrawData {
  uint material;
  float fade;
  uint flags;  // bit0 invert the dissolve mask, bit1 hold animation at rest, bit2 has normals
  uint uvSlot;
  uint tint;   // editor tint (ghost/selection) as RGBA8; white otherwise
};
layout(std430, binding = 5) readonly buffer Draws { DrawData draws[]; };
#define materialId int(draws[drawIndex].material)
#define lodFade draws[drawIndex].fade
#define lodFadeInvert int(draws[drawIndex].flags & 1u)
#define animStill int((draws[drawIndex].flags >> 1) & 1u)
#define hasNormals int((draws[drawIndex].flags >> 2) & 1u)
#define drawTint unpackUnorm4x8(draws[drawIndex].tint)
)";

// Handles are uvec2 (std430-safe) and must stay dynamically uniform: indexed only
// through the per-draw materialId (gl_DrawIDARB is uniform per sub-draw) or, in the
// visibility resolve, the work group's material. Never 0 (see GpuMaterial).
constexpr const char* kMaterialGlsl = R"(
struct Material {
  uvec2 albedoMap, metalMap, normalMap, smoothMap, emissiveMap, detailDiffuseMap,
      detailNormalMap, paintMap, dirtMap, heightMap, glowDetailMap, glowDetail2Map,
      multimatDiffuse, multimatNormal, multimatHeight;
  vec2 glowAdjust;
  vec2 mrFactor;      // metallic(x) / 1-smoothness(y) seeds; the maps multiply in
  vec2 detailTiling;  // x = diffuse detail UV tiling, y = normal detail
  vec2 detailStr;     // x = diffuse detail strength, y = normal detail
  float glowStr;      // X4 color_glowStr: emissive scale (0 = glow off)
  float diffuseStr;   // HDR multiplier, only meaningful when additiveUnlit
  vec4 glowDetailUv;  // UV0 tiling u, v, scroll speed u, v
  vec4 glowDetail2Uv;
  vec4 glowDetailMix;  // layer strengths, animglow baseglow_strength, layerweight
  vec3 hullLayerStr;   // paint strength, dirt strength, dirt tiling
  float colorHeightCutoff;
  vec3 pomParams;      // height strength, midlevel, aspect ratio
  uint pomLayerOffset;
  int useSmoothMap, normalTwoChannel, useNormalMap, useMetalMap, useEmissiveMap;
  int alphaTest;      // Mask: discard transparent texels (the greeble fix)
  int additiveUnlit;  // Additive: X4 draws these with unlit shaders
  int opaquePass;     // non-blended draw: write alpha 1 (blended draws keep coverage)
  int useDiffuseDetail, useNormalDetail, usePaint, useDirt;
  int hullLayers;     // HullLayers: 0 p1, 1 p2 surface, 2 boron, 3 p1 / 4 p2 decal
  int useHeightMap;
  int useMultimat;    // Multimat: 0 none, 1 p1, 2 p2
  int useMultimatDiffuse, useMultimatNormal, useMultimatHeight;
  int glowDetail;     // GlowDetail: 0 none, 1 multiply, 2 boron animglow
};
layout(std430, binding = 2) readonly buffer Materials { Material materials[]; };
layout(std430, binding = 3) readonly buffer PomLayers { float pomLayers[]; };  // multimat layerN_str
)";

constexpr const char* kUnlitMaterialGlsl = R"(
struct UnlitMaterial {
  uvec2 diffuseMap;
  uvec2 layerMaps[4];  // diffuse2, diffuse3, alpha1, alpha2
  vec2 softRange;      // U_softparticle_range_min, _max
  vec4 diffuseColor;
  vec4 colorEmissive;
  vec4 grade;          // brightness, contrast, saturation, hue
  vec4 cameraFade;     // near start, near end, far start, far end
  vec4 layerStr;
  vec2 angleFade;      // speed, offset
  vec2 dblend;
  float diffuseStr;
  float userAlphaScale;
  int hasDiffuse, vertexColors, isForcefield, opaque;
  int layerMask;  // bit k = layerMaps[k] loaded
  int lightcone;  // 1 xu_lightcone, 2 _env, 3 _alpha: cameraFade = near range, speed, far start, stop
};
layout(std430, binding = 4) readonly buffer UnlitMaterials { UnlitMaterial unlitMaterials[]; };
)";

// The lit station fragment stage. kFrameDataGlsl and kMaterialGlsl follow this head
// (flatFsSource).
constexpr const char* kFlatFsHead = R"(#version 430
#extension GL_ARB_shader_ballot : require
#extension GL_ARB_sample_shading : require
#extension GL_ARB_bindless_texture : require

// Centroid: under MSAA a pixel whose centre misses the triangle still shades, and
// centre-interpolated position/normal/uv extrapolate past the edge (bright local-light
// and specular specks along every hull edge).
centroid in vec2 fragTexCoord;
centroid in vec2 fragDirtUv;
centroid in vec2 fragPaintUv;
centroid in vec4 fragColor;
centroid in vec3 fragPosition;
centroid in vec3 fragNormal;
// X4 IO_damage_detail_age: paint-mod mask, ao, dirt amount, p2 dirt amount; multimat
// reads xyz as smoothness, metalness, detail-array layer (p2: DATA2, see prepareStationPart).
in vec4 fragDamageDetail;
flat in int vDrawIndex;
// The sub-draw's index is uniform across the wave, but a flat input doesn't tell the
// compiler so: without the broadcast every bindless material read is a waterfall loop.
#define drawIndex readFirstInvocationARB(vDrawIndex)
// shadeSurface's hooks: the forward stage has hardware derivatives and runs the cutout.
#define SHADE_FORWARD
#define SAMPLE(s, uv, dx, dy) texture(s, uv)
#define NORMAL_DX(n) dFdx(n)
#define NORMAL_DY(n) dFdy(n)
int coverage;  // this fragment's sample mask; the cutout narrows it
)";

// The lit shading body (shadeSurface) and its helpers. Every input arrives in a
// Surface with its screen-space gradients, so a stage without hardware derivatives
// can run it; the head defines SAMPLE (implicit or explicit-gradient fetch).
constexpr const char* kFlatFsBody = R"(
struct Surface {
  vec3 position;  // X4 world space (the display flip lives in mvp)
  vec3 normal;    // interpolated vertex normal, valid when hasNormals
  vec2 uv0;       // UV map 0, scroll-animated
  vec2 uv1;       // UV map 1: p1_multimat's dirt UV
  vec2 uv2;       // UV map 2: the paint UV
  vec4 color;     // COLOR0, raw: rgb tint, a = dirt amount
  vec4 data;      // DATA1 (p2_multimat: DATA2) / 255
  vec3 dPdx, dPdy;
  vec2 duv0dx, duv0dy, duv1dx, duv1dy, duv2dx, duv2dy;
  vec2 pixelCoord;  // window coordinates (the AO and light-grid lookups)
};
layout(binding = 16) uniform sampler2DShadow shadowMap0;
layout(binding = 17) uniform sampler2DShadow shadowMap1;
layout(binding = 18) uniform sampler2DShadow shadowMap2;
layout(binding = 19) uniform sampler2DShadow shadowMap3;
layout(binding = 10) uniform sampler2DShadow shadowMap4;
layout(binding = 11) uniform sampler2D aoMap;  // Ssao, 1 = unoccluded
layout(binding = 9) uniform samplerCube envMap;  // sky-dome bake (EnvCubemap); 1x1 black fallback
// Clustered authored lights (local_lights.hpp): 64x36 tiles x 64 log-depth slices,
// 6 texels per light record; texel 0 = position + reach, so a light out of reach
// costs one fetch.
layout(binding = 12) uniform samplerBuffer lightData;
layout(binding = 13) uniform usamplerBuffer lightGrid;
layout(binding = 14) uniform usamplerBuffer lightIndex;
// This draw's material and texture clock, set first thing in main().
Material mat;
float glowTime;

// Overlay keyed on the base: multiply below A~0.45, screen above, over a 10-wide
// soft knee; these constants keep smooth/metal detail matching the game.
float blendOverlay(float A, float B) {
  float L = clamp(10.0 * (A - 0.45), 0.0, 1.0);
  float result1 = 2.0 * A * B;
  float result2 = 1.0 - 2.0 * (1.0 - B) * (1.0 - A);
  return mix(result1, result2, L);
}

// X4's make_ColorMatrix hue term: rotation about the grey axis.
vec3 hueRotate(vec3 rgb, float theta) {
  vec3 axis = vec3(0.57735026919);
  return rgb * cos(theta) - cross(axis, rgb) * sin(theta)
       + axis * dot(axis, rgb) * (1.0 - cos(theta));
}

// X4's parallax occlusion mapping, ported from the shipped builds (X4_SHOWROOM: hull draw
// 44243 = complex_surface_pom, pad draw 45042 = p2 multimat; p1 multimat from its plain
// source). Explicit gradients: an implicit-derivative fetch inside the march's divergent
// loop is undefined.
float pomHeight(vec2 uv, float layer, vec2 dx, vec2 dy) {
  return mat.useMultimat != 0 ? textureGrad(sampler2DArray(mat.multimatHeight), vec3(uv, layer), dx, dy).r
                          : textureGrad(sampler2D(mat.heightMap), uv, dx, dy).r;
}

// The weight X4 saturates; the guard keeps its 0/0 case from turning the UV into NaN.
float pomWeight(float after, float before) {
  return after == before ? 0.0 : clamp(after / (after - before), 0.0, 1.0);
}

// viewTs = the unit view vector in X4's tangent frame: our T is its -tangent (CalcWorldNormal),
// cancelling the x negation, and its binormal is our -B for the march (the display Z mirror
// flips our cross-product B; A/B vs the captures: +B worsens every term, -B improves them).
// The offset is not divided by z.
vec2 pomUv(vec2 uv, float layer, float layerZ, vec3 viewTs, vec2 dx, vec2 dy) {
  float n = mix(64.0, 16.0, abs(viewTs.z));  // "high": F_pom_maxlayers, F_pom_minlayers
  float layerDepth = 1.0 / n;
  float strength = 0.015 * mat.pomParams.x;
  // X4 indexes layerN_str by z x 256 (not the array layer's x 255); the clamp stops its
  // out-of-range read at z = 1.
  if (mat.useMultimat != 0) strength *= pomLayers[mat.pomLayerOffset + uint(clamp(int(layerZ * 256.0), 0, 63))];
  vec2 delta = viewTs.xy * vec2(1.0, mat.pomParams.z) * strength / n;
  if (mat.useMultimat == 1) {
    // p1 multimat: the classic one-way march into depth = 1 - height.
    float depth = 1.0 - pomHeight(uv, layer, dx, dy);
    float layerD = 0.0;
    for (int i = 0; i <= int(n) && layerD < depth; ++i) {
      uv -= delta;
      depth = 1.0 - pomHeight(uv, layer, dx, dy);
      layerD += layerDepth;
    }
    vec2 prev = uv + delta;
    float after = depth - layerD;
    float before = 1.0 - pomHeight(prev, layer, dx, dy) - layerD + layerDepth;
    float w = after == before ? 0.0 : after / (after - before);  // X4 p1 leaves it unclamped
    return mix(uv, prev, w);
  }
  // complex_surface_pom / p2 multimat: march up or down from the midlevel until the layer
  // meets the height map.
  float h = pomHeight(uv, layer, dx, dy);
  float layerH = mat.pomParams.y;
  for (int i = 0; i < int(n) && abs(layerH - h) > layerDepth + 0.0015; ++i) {
    float s = sign(layerH - h);
    uv -= delta * s;
    h = pomHeight(uv, layer, dx, dy);
    layerH -= layerDepth * s;
  }
  float s = sign(layerH - h);
  float after = h - layerH;
  if (mat.useMultimat == 2) {
    vec2 prev = uv - delta * s;
    return mix(uv, prev, pomWeight(after, pomHeight(prev, layer, dx, dy) - layerH));
  }
  vec2 prev = uv + delta * s;
  return mix(uv, prev, pomWeight(after, pomHeight(prev, layer, dx, dy) - layerH - layerDepth * s));
}

// Sun visibility = X4's GetShadow / GetCSMShadow (common_shadows.glsl, matched to
// the shipped global pass decompiled from X4_Station_FRONT.rdc). Each cascade's
// map and depth range are the same cube, so X4's depth-unit biases carry over
// unchanged. I_pcfmode 1 = Castano's kernels as bilinear compare taps: 7x7 (16
// taps) for c0-c2, 5x5 (9 taps) for c3-c4.
float pcf7(sampler2DShadow map, vec3 p, float texel) {
  vec2 uv = p.xy / texel + 0.5;
  vec2 base = floor(uv);
  vec2 s = uv - base;
  base = (base - 0.5) * texel;
  vec4 uw = vec4(5.0 * s.x - 6.0, 11.0 * s.x - 28.0, -(11.0 * s.x + 17.0), -(5.0 * s.x + 1.0));
  vec4 vw = vec4(5.0 * s.y - 6.0, 11.0 * s.y - 28.0, -(11.0 * s.y + 17.0), -(5.0 * s.y + 1.0));
  vec4 u = vec4((4.0 * s.x - 5.0) / uw.x - 3.0, (4.0 * s.x - 16.0) / uw.y - 1.0,
                -(7.0 * s.x + 5.0) / uw.z + 1.0, -s.x / uw.w + 3.0);
  vec4 v = vec4((4.0 * s.y - 5.0) / vw.x - 3.0, (4.0 * s.y - 16.0) / vw.y - 1.0,
                -(7.0 * s.y + 5.0) / vw.z + 1.0, -s.y / vw.w + 3.0);
  float lit = 0.0;
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i)
      lit += uw[i] * vw[j] * texture(map, vec3(base + vec2(u[i], v[j]) * texel, p.z));
  return lit / 2704.0;
}

float pcf5(sampler2DShadow map, vec3 p, float texel) {
  vec2 uv = p.xy / texel + 0.5;
  vec2 base = floor(uv);
  vec2 s = uv - base;
  base = (base - 0.5) * texel;
  vec3 uw = vec3(-(3.0 * s.x - 4.0), 7.0, 3.0 * s.x + 1.0);
  vec3 vw = vec3(-(3.0 * s.y - 4.0), 7.0, 3.0 * s.y + 1.0);
  vec3 u = vec3(-(2.0 * s.x - 3.0) / uw.x - 2.0, (3.0 + s.x) / uw.y, s.x / uw.z + 2.0);
  vec3 v = vec3(-(2.0 * s.y - 3.0) / vw.x - 2.0, (3.0 + s.y) / vw.y, s.y / vw.z + 2.0);
  float lit = 0.0;
  for (int j = 0; j < 3; ++j)
    for (int i = 0; i < 3; ++i)
      lit += uw[i] * vw[j] * texture(map, vec3(base + vec2(u[i], v[j]) * texel, p.z));
  return lit / 144.0;
}

vec3 csmCoords(mat4 lightVP, vec3 P) {
  vec4 lc = lightVP * vec4(P, 1.0);
  return lc.xyz / lc.w * 0.5 + 0.5;
}

// X4's GetScaleBias, subtracted from all three coords as X4 does. X4's map v
// runs opposite to our y (D3D vs GL texture origin), hence +b on y.
vec3 csmBias(float epsilon, float maxBias, float ndl) {
  float b = clamp(epsilon * sqrt(max(1.0 - ndl * ndl, 0.0)) / ndl, 0.0, maxBias);
  return vec3(b, -b, b);
}

// X4's getCSMWeights: a linear pyramid, 1 inside, falling to 0 over the outer
// sixth of the cube on every axis.
float csmWeight(vec3 c) {
  vec3 w = min(c, 1.0 - c) * 6.0;
  return clamp(min(w.x, min(w.y, w.z)), 0.0, 1.0);
}

// X4's shipped global pass (station.rdc, decompiled fs of draw 26311) fades the
// sun shadow by camera DISTANCE from 0.9 to 1.4 x F_shadowmaxdistance (10 km):
// full shadow to 9 km, none past 14 km. common_shadows.glsl's 9-10 km view-depth
// fade is not the variant that ships.
float sunShadow(vec3 P, float ndl) {
  // At n.l = 0 the caller multiplies the result away: skip the taps.
  if (shadowCascades == 0 || ndl <= 0.0) return 1.0;
  const float kShadowRange = 10000.0;
  float fade = clamp((length(P - cameraPos) - 0.9 * kShadowRange) / (0.5 * kShadowRange), 0.0, 1.0);
  if (fade >= 1.0) return 1.0;

  vec3 c0 = csmCoords(lightVP0, P) - csmBias(0.005, 1e-4, ndl);
  vec3 c1 = csmCoords(lightVP1, P);
  vec3 c2 = csmCoords(lightVP2, P);
  vec3 c3 = csmCoords(lightVP3, P);
  vec4 wt = vec4((shadowCascades & 1) != 0 ? csmWeight(c0) : 0.0,
                 (shadowCascades & 2) != 0 ? csmWeight(c1) : 0.0,
                 (shadowCascades & 4) != 0 ? csmWeight(c2) : 0.0,
                 (shadowCascades & 8) != 0 ? csmWeight(c3) : 0.0);
  float w1 = min(1.0 - wt.x, wt.y);
  float w2 = min(1.0 - wt.y, wt.z);
  float w3 = min(1.0 - wt.z, wt.w);
  float w4 = 1.0 - clamp(wt.x + w1 + w2 + w3, 0.0, 1.0);
  float lit = 0.0;
  if (wt.x > 0.0) lit += wt.x * pcf7(shadowMap0, c0, shadowTexel0);
  if (w1 > 0.0) lit += w1 * pcf7(shadowMap1, c1 - csmBias(0.005, 2e-5, ndl), shadowTexel1);
  if (w2 > 0.0) lit += w2 * pcf7(shadowMap2, c2 - csmBias(0.5, 2e-4, ndl), shadowTexel2);
  if (w3 > 0.0) lit += w3 * pcf5(shadowMap3, c3 - csmBias(5e-4, 3e-5, ndl), shadowTexel3);
  if (w4 > 0.0) {
    // Our far map is centred on the station: outside it there is nothing to cast.
    vec3 c4 = csmCoords(lightVP4, P);
    bool inside = (shadowCascades & 16) != 0 && all(greaterThanEqual(c4.xy, vec2(0.0))) &&
                  all(lessThanEqual(c4.xy, vec2(1.0))) && c4.z <= 1.0;
    lit += w4 * (inside ? pcf5(shadowMap4, c4 - csmBias(5e-5, 1e-3, ndl), shadowTexel4) : 1.0);
  }
  return clamp(fade + lit, 0.0, 1.0);
}

// ---- X4's local light passes (shadergl/glsl/p1/high/l_pass_*.frag.glsl), ported
// from deferred light volumes to a forward loop. `a` is roughness squared.
const float kPi = 3.14159265;

// EvalBRDF (p1/deferred/lighting_common.glsl); it re-applies the reflectance
// energy term to cdiff, as X4 does.
vec3 x4Brdf(vec3 cspec, vec3 cdiff, float a, vec3 l, vec3 v, vec3 n, vec2 mask) {
  vec3 h = normalize(v + l);
  float nl = clamp(dot(n, l), 0.0, 1.0);
  float nv = clamp(abs(dot(n, v)) + 1e-8, 0.0, 1.0);
  float nh = clamp(dot(n, h), 0.0, 1.0);
  float vh = clamp(dot(v, h), 0.0, 1.0);
  float a2 = a * a;
  float d = (nh * a2 - nh) * nh + 1.0;
  float D = a2 / (kPi * d * d);
  float Vis = 0.5 / max(1e-8, nl * (nv * (1.0 - a) + a) + nv * (nl * (1.0 - a) + a));
  float f = pow(1.0 - vh, 5.0);
  vec3 F = f + (1.0 - f) * cspec;
  vec3 cd = cdiff * clamp(1.0 - dot(vec3(0.299, 0.587, 0.114), cspec), 0.0, 1.0);
  return cd / kPi * mask.x + D * Vis * F * mask.y;
}

// EvalBRDFSimpleSpec.
vec3 x4SimpleSpec(vec3 cspec, float a, vec3 l, vec3 v, vec3 n) {
  float nh = clamp(dot(n, normalize(v + l)), 0.0, 1.0);
  float a2 = a * a;
  float d = (nh * a2 - nh) * nh + 1.0;
  return a2 / (kPi * d * d) * 0.25 * cspec;
}

// HLSL smoothstep, which the spot cone calls with edge0 > edge1 (undefined in GLSL).
float x4Smoothstep(float e0, float e1, float x) {
  float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

// Representative points for area-light specular (Karis 2013).
// Every vector is relative to the shading point; dir is the unit reflection ray.
// Point on the segment c + s * axis (|s| <= h) seen at the smallest angle from dir.
vec3 segmentRPM(vec3 dir, vec3 c, vec3 axis, float h) {
  float ca = dot(c, axis);
  float rc = dot(dir, c);
  float ra = dot(dir, axis);
  float den = ca * ra - dot(axis, axis) * rc;
  // A zero denominator (ray parallel to the line) would feed NaN into clamp().
  float s = abs(den) > 1e-12 ? (ca * rc - dot(c, c) * ra) / den : 0.0;
  return c + clamp(s, -h, h) * axis;
}

vec3 planeRPM(vec3 dir, vec3 L, vec2 lsize, vec3 ldir, vec3 right, vec3 up) {
  vec3 hit = dir * (dot(L, ldir) / dot(dir, ldir));
  vec3 off = hit - L;
  if (abs(dot(off, right)) < lsize.x && abs(dot(off, up)) < lsize.y) return hit;
  vec3 best = L;
  float bestCos = 0.0;
  for (int e = 0; e < 4; ++e) {
    bool vertical = e < 2;
    float sgn = (e & 1) == 0 ? 1.0 : -1.0;
    vec3 c = L + sgn * (vertical ? lsize.x * right : lsize.y * up);
    vec3 p = vertical ? segmentRPM(dir, c, up, lsize.y) : segmentRPM(dir, c, right, lsize.x);
    float cosA = dot(normalize(p), dir);
    if (cosA > bestCos) {
      bestCos = cosA;
      best = p;
    }
  }
  return best;
}

vec3 tubeRPM(vec3 dir, vec3 L, vec2 lsize, vec3 right, vec3 up) {
  return lsize.x > lsize.y ? segmentRPM(dir, L, right, lsize.x) : segmentRPM(dir, L, up, lsize.y);
}

vec3 sphereRPM(vec3 dir, vec3 L, float lradius) {
  vec3 toRay = dot(L, dir) * dir - L;
  return L + toRay * clamp(lradius / length(toRay), 0.0, 1.0);
}

// X4's local lights attenuate only their diffuse by the SSAO (l_pass_pointlight).
vec3 localLights(vec3 P, vec3 N, vec3 V, vec3 cspec, vec3 cdiff, float a, float ao, vec2 pixel) {
  float z = max(-(lightView * vec4(P, 1.0)).z, 1e-6);
  int s = clamp(int(floor(log(z) * sliceParams.x + sliceParams.y)), 0, 63);
  ivec2 t = clamp(ivec2(pixel / clusterViewport * vec2(64.0, 36.0)), ivec2(0), ivec2(63, 35));
  uvec2 cl = texelFetch(lightGrid, (s * 36 + t.y) * 64 + t.x).xy;
  vec3 sum = vec3(0.0);
  for (uint k = 0u; k < cl.y; ++k) {
    int base = int(texelFetch(lightIndex, int(cl.x + k)).r) * 6;
    vec4 t0 = texelFetch(lightData, base);
    vec3 toL = t0.xyz - P;
    if (dot(toL, toL) > t0.w * t0.w) continue;
    vec4 t1 = texelFetch(lightData, base + 1);
    vec4 t2 = texelFetch(lightData, base + 2);
    vec4 t3 = texelFetch(lightData, base + 3);
    vec4 t4 = texelFetch(lightData, base + 4);
    int kind = int(t4.w);
    vec3 clr = t1.rgb;
    float spec = t1.w;
    if (kind == 0) {  // omni: l_pass_pointlight
      vec3 L = t0.xyz - P;
      float d = length(L);
      float R = t2.w;
      if (d >= R * 0.9) continue;
      vec3 l = L / max(d, 1e-6);
      float ndl = clamp(dot(l, N), 0.0, 1.0);
      if (ndl <= 0.0) continue;
      float win = pow(clamp(1.0 - pow(d / (R * 0.9), 4.0), 0.0, 1.0), 2.0);
      float att = clamp(win / (1.0 / max(d * d, 1e-8) + 1.0), 0.0, 1.0);
      sum += x4Brdf(cspec, cdiff, a, l, V, N, vec2(ao, spec)) * clr * ndl * att;
    } else if (kind == 1) {  // spot: l_pass_spotlight
      vec3 dv = P - t0.xyz;
      float d = length(dv);
      float R = t2.w;
      if (d > R || d <= 0.0) continue;
      float q = R / d;
      float att = max(0.0, (1.0 - 1.0 / pow(q, 0.2)) * pow(q, 1.4));
      float ang = degrees(acos(clamp(dot(dv / d, t2.xyz), -1.0, 1.0)));
      float radial = pow(x4Smoothstep(t3.w * 0.95, 0.0, ang), 0.5);
      if (radial <= 0.0) continue;
      vec3 l = normalize(-(dv - N * d * 0.5));
      float ndl = clamp(dot(N, l), 0.0, 1.0);
      if (ndl <= 0.0) continue;
      vec3 c = x4Brdf(cspec, cdiff, a, l, V, N, vec2(ao, spec)) * clr * ndl
             + spec * clr * ndl * x4SimpleSpec(cspec, a, l, V, N);
      sum += clamp(c * radial * att, 0.0, 2.0);
    } else if (kind == 2) {  // box: a line light, l_pass_boxlight
      float R = t2.w;
      vec3 l1 = t0.xyz + t2.xyz * t3.w;
      vec3 seg = -2.0 * t2.xyz * t3.w;
      float len2 = max(dot(seg, seg), 1e-8);
      float tt = clamp(-dot(l1 - P, seg) / len2, 0.0, 1.0);
      vec3 L = l1 + tt * seg - P;
      float d = length(L);
      if (d > sqrt(len2) + R) continue;
      float att = clamp(1.0 - pow(d / (R * 0.9), 2.0), 0.0, 1.0);
      if (att <= 0.0) continue;
      vec3 l = L / max(d, 1e-6);
      float ndl = clamp(dot(N, l), 0.0, 1.0);
      if (ndl <= 0.0) continue;
      sum += x4Brdf(cspec, cdiff, a, l, V, N, vec2(ao, spec)) * clr * ndl * att;
    } else {  // area: l_pass_arealight_gen
      vec4 t5 = texelFetch(lightData, base + 5);
      vec3 ldir = t2.xyz;
      float range = t2.w;
      vec3 right = t3.xyz;
      float lr = t3.w;
      vec3 up = t4.xyz;
      vec2 hs = t5.xy;
      vec3 tolight = t0.xyz - P;
      float dist = length(tolight);
      if (dist > lr + length(hs) + range * 2.0) continue;
      vec3 lraw = tolight;
      if (hs.x + hs.y > 0.0) {
        vec2 uv = clamp(vec2(dot(-tolight, right), dot(-tolight, up)), -hs, hs);
        lraw = tolight + uv.x * right + uv.y * up;
      }
      vec3 ldiff = normalize(lraw);
      float dst = max(0.0, length(lraw) - lr);
      float att = clamp(1.0 - dst / range, 0.0, 1.0) / (1.0 + dst * dst);
      float sp0 = clamp(t5.z, 0.0, 1.0) * 0.99;
      if (sp0 > 0.0) {
        float da = clamp((0.5 * dot(-ldiff, ldir) + 0.5 - sp0) / (1.0 - sp0), 0.0, 1.0);
        att *= da * da;
      }
      if (att <= 0.0) continue;
      vec3 c = clr * clamp(dot(N, ldiff), 0.0, 1.0) * cdiff / kPi;
      float rough = max(sqrt(a), 0.05);
      float ra = rough * rough;
      float norm = 1.0 / (kPi * ra * ra);
      vec3 rdir = reflect(-V, N);
      vec3 Lp = tolight;
      if (min(hs.x, hs.y) > 0.001)
        Lp = planeRPM(rdir, Lp, hs, ldir, right, up);
      else if (max(hs.x, hs.y) > 0.001)
        Lp = tubeRPM(rdir, Lp, hs, right, up);
      if (lr > 0.0) Lp = sphereRPM(rdir, Lp, lr);
      float specatt = 1.0 / (1.0 + norm * clamp((hs.x + hs.y + lr) / max(dist, 1e-6), 0.0, 1.0));
      vec3 Ln = normalize(Lp);
      float nl = clamp(dot(Ln, N), 0.0, 1.0);
      float hz = 1.0 - nl;
      hz *= hz;
      specatt -= specatt * hz * hz;
      c += spec * clr * specatt * nl * x4Brdf(cspec, cdiff, ra, Ln, V, N, vec2(0.0, 1.0));
      sum += clamp(c * att, 0.0, 2.0);
    }
  }
  return sum;
}

)";

// LOD dissolve (X4 fades levels the same way): a 4x4 Bayer screen-door, with the
// pair's two levels on complementary masks so every sample is covered exactly
// once. Sample i ranks i*16 + bayer, so under MSAA a pixel's samples split
// between the two levels and the resolve blends them instead of showing the
// pattern; with one sample it is the plain 4x4 screen-door. Shared by the lit
// pass and the depth prepass (also the visibility geometry pass).
constexpr const char* kLodCoverageGlsl = R"(
int lodCoverage() {
  const int bayer[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
  ivec2 p = ivec2(gl_FragCoord.xy) & 3;
  int b = bayer[p.x + p.y * 4];
  int n = min(gl_NumSamples, 16);
  if (n == 1) {
    int single = (float(b) + 0.5) / 16.0 < lodFade ? 1 : 0;
    return lodFadeInvert == 1 ? 1 - single : single;
  }
  // Sample 0 carries the dominant level on every pixel, so the one-sample depth
  // resolve SSAO reads never sees the dissolve's screen-door (a checkerboard of two
  // meshes there read as occlusion); the other samples dither the rest of the coverage.
  bool first = lodFade >= 0.5;
  float rest = (lodFade * float(n) - (first ? 1.0 : 0.0)) / float(n - 1);
  int mask = first ? 1 : 0;
  for (int i = 1; i < n; ++i)
    if ((float((i - 1) * 16 + b) + 0.5) / float(16 * (n - 1)) < rest) mask |= 1 << i;
  if (lodFadeInvert == 1) mask = ~mask & ((1 << n) - 1);
  return mask;
}
)";

// The Mask cutout as alpha-to-coverage: the edge spreads over the samples by its screen
// slope; with one sample this is exactly the a < ref cut. The p2 surface/boron builds cut
// at their F_alpha_ref spec constant (60/255 in every capture), the decal and p1 shaders
// at a fixed 0.5. Fragment stages only (fwidth).
constexpr const char* kCutoutGlsl = R"(
int cutoutCoverage(float cut, bool p2Layers) {
  float ref = p2Layers ? 0.2352941 : 0.5;
  float a = clamp((cut - ref) / max(fwidth(cut), 1e-4) + 0.5, 0.0, 1.0);
  int n = min(gl_NumSamples, 16);
  int mask = 0;
  for (int i = 0; i < n; ++i)
    if (a > (float(i) + 0.5) / float(n)) mask |= 1 << i;
  return mask;
}
)";

// Expects mat, glowTime (and in the forward stage, coverage) set by the caller.
constexpr const char* kShadeSurfaceGlsl = R"(
vec4 shadeSurface(Surface s) {
  // Reconstruct geometric normal and tangent space
  vec3 Q1  = s.dPdx;
  vec3 Q2  = s.dPdy;
  vec2 st1 = s.duv0dx;
  vec2 st2 = s.duv0dy;

  vec3 Ng  = normalize(cross(Q1, Q2));
  vec3 N   = Ng;
  // Real vertex normals when the mesh ships them (X4 shades smooth); the
  // derivative fallback keeps normal-less meshes lit.
  bool vertexNormal = hasNormals == 1 && dot(s.normal, s.normal) > 1e-12;
  if (vertexNormal) N = normalize(s.normal);
  vec3 V   = normalize(cameraPos - s.position);
  // Unculled ranges (cutouts, TWOSIDED) show their back faces: X4's INPUT_NTB_TWOSIDED
  // flips only the normal there, keeping the front's tangent frame. Facing = the vertex
  // normal against the viewer-facing triangle plane (the resolve has no gl_FrontFacing).
  bool backFace = vertexNormal && dot(N, Ng) * dot(Ng, V) < 0.0;
  if (dot(N, V) < 0.0 && !backFace) N = -N;

  vec3 T = normalize(Q1*st2.t - Q2*st1.t);
  if (hasNormals == 1) {
      // The derivative tangent lies in the triangle plane, not necessarily
      // perpendicular to the interpolated normal — re-orthogonalize. Gated:
      // for the derivative-N path this is an exact no-op, and gating keeps
      // that path byte-stable.
      T = normalize(T - N * dot(N, T));
  }
  // The derivative tangent carries the uv Jacobian's sign, which a back face mirrors.
  if (backFace) T = -T;
  vec3 B = -normalize(cross(N, T));
  if (backFace) N = -N;
  mat3 TBN = mat3(T, B, N);

  // GL rounds and clamps an array layer coordinate, as X4's float index relies on.
  float layer = s.data.z * 255.0;
  // Every UV0 map reads the parallax-shifted uv; paint keeps its own UV, as in X4.
  vec2 uv = s.uv0;
  bool pomMap = mat.useMultimat != 0 ? mat.useMultimatHeight == 1 : mat.useHeightMap == 1;
  if (usePom == 1 && pomMap && mat.pomParams.x != 0.0)
      uv = pomUv(uv, layer, s.data.z, transpose(TBN) * V * vec3(1.0, -1.0, 1.0), st1, st2);

  vec3 ntex = vec3(0.5, 0.5, 1.0);
  if (mat.useNormalMap == 1) {
      ntex = SAMPLE(sampler2D(mat.normalMap), uv, st1, st2).rgb;
  }
  if (mat.useMultimat != 0) {
      // p1/p2 multimat.frag.glsl: the layer's detail normal (z rebuilt from xy) replaces
      // the vertex normal; without the array the vertex normal stands.
      if (mat.useMultimatNormal == 1) {
          vec2 dn = SAMPLE(sampler2DArray(mat.multimatNormal), vec3(uv, layer), st1, st2).xy * 2.0 - 1.0;
          dn.y *= float(normalBinormalSign);
          N = normalize(TBN * vec3(dn, sqrt(clamp(1.0 - dot(dn, dn), 0.0, 1.0))));
      }
  } else if (length(ntex) > 0.0) {
      vec3 tanN = ntex * 2.0 - 1.0;
      if (mat.normalTwoChannel == 1 && mat.useNormalMap == 1)
          tanN.z = sqrt(max(0.0, 1.0 - dot(tanN.xy, tanN.xy)));
      // X4 adds the detail map's signed XY before renormalizing (common.glsl
      // SAMPLE_NORMAL_MAP2) — and only when a base normal map is in use.
      if (mat.useNormalMap == 1 && mat.useNormalDetail == 1) {
          vec2 dn = SAMPLE(sampler2D(mat.detailNormalMap), uv * mat.detailTiling.y,
                         st1 * mat.detailTiling.y, st2 * mat.detailTiling.y).xy * 2.0 - 1.0;
          tanN = normalize(vec3(tanN.xy + dn * mat.detailStr.y, tanN.z));
      }
      tanN.y *= float(normalBinormalSign);
      N = normalize(TBN * tanN);
  }

  vec4 texel = SAMPLE(sampler2D(mat.albedoMap), uv, st1, st2);
  // The multimat layer's channel-packed detail: R albedo, G smoothness, B metalness;
  // p2 cuts out where A < 0.5 (its own discard, X4 forces the texture alpha to 1).
  vec4 mmDetail = vec4(1.0);
  if (mat.useMultimat != 0 && mat.useMultimatDiffuse == 1)
      mmDetail = SAMPLE(sampler2DArray(mat.multimatDiffuse), vec3(uv, layer), st1, st2);
  bool p2Layers = mat.hullLayers == 1 || mat.hullLayers == 2;  // p2 surface, boron
#ifdef SHADE_FORWARD
  if (mat.alphaTest == 1) {
    coverage &= cutoutCoverage(mat.useMultimat == 2 ? mmDetail.a : texel.a, p2Layers);
    if (coverage == 0) discard;
    gl_SampleMask[0] = coverage;
  }
#endif
  // Viewshot-only direct normal dump for tangent-frame A/Bs (mask 256).
  // Keep it after the material cutout so the diagnostic matches visible surfaces.
  if (lightMask == 256) {
    return vec4(N * 0.5 + 0.5, 1.0);
  }
  // X4 sRGB-decodes the u8 vertex colour and multiplies the diffuse by it, full
  // stop (GetVertexColor, common_vert.glsl; verified in station.rdc: hull verts
  // carry 255 = 1.0, and the LUT tonemap applies no exposure). The old x4.5554
  // "neutral 128 = 1.0" normalisation had no counterpart in-game and put every
  // hull ~4.5x over the sky/sun energy the frame is calibrated against.
  vec3 tint = pow(s.color.rgb, vec3(2.2));

  if (mat.additiveUnlit == 1) {
    // Linear HDR out (tonemap lives in the composite); alpha pre-multiplied,
    // additive blend weight 1.
    return vec4(texel.rgb * drawTint.rgb * tint * mat.diffuseStr * texel.a, 1.0);
  }

  // p2 surface/boron: the vertex colour keeps its hue only above the height cutoff and
  // is greyed below it (no height map: X4's height is 1). With POM off X4's height is 1 too;
  // sampling it unshifted keeps the toggle a parallax-only A/B.
  float height = mat.useHeightMap == 1 ? SAMPLE(sampler2D(mat.heightMap), uv, st1, st2).r : 1.0;
  if (p2Layers) {
      float grey = clamp(dot(tint, vec3(0.3086, 0.6094, 0.0820)), 0.0, 1.0);
      tint = mix(vec3(grey), tint, clamp(clamp(height - mat.colorHeightCutoff, 0.0, 1.0) * 2.16, 0.0, 1.0));
  }
  // texture0 is an sRGB texture, like X4's view: decoded before filtering.
  vec3 albedo = texel.rgb * drawTint.rgb * tint;

  // X4's scalar Metallness/Smoothness seed the terms and their maps multiply in
  // (mrFactor.y carries 1 - smoothness).
  float metallic = mat.mrFactor.x;
  float smoothness = 1.0 - mat.mrFactor.y;
  if (mat.useMetalMap == 1) metallic *= SAMPLE(sampler2D(mat.metalMap), uv, st1, st2).r;
  if (mat.useSmoothMap == 1) smoothness *= SAMPLE(sampler2D(mat.smoothMap), uv, st1, st2).r;
  metallic = clamp(metallic, 0.0, 1.0);
  // multimat (matches the shipped p1/p2 decompiles): DATA1 (p2: DATA2) seeds smoothness
  // and metalness per vertex, and the layer's detail multiplies all three terms (no
  // overlay, unlike complex_surface).
  if (mat.useMultimat != 0) {
      smoothness = s.data.x * mmDetail.g;
      metallic = s.data.y * mmDetail.b;
      albedo *= mmDetail.r;
  }

  // X4's detail blend (complex_surface.frag.glsl): the map is CHANNEL-PACKED,
  // R multiplies the diffuse, G/B overlay-blend SMOOTHNESS and metalness. X4
  // binds it through an sRGB view (station.rdc: BC1_SRGB, uploaded the same), so
  // the ~0.5 G/B masks reach the overlay as ~0.22 and roughen the hull (G-buffer
  // roughness 0.54, linear sampling gave 0.34: the glossy "lamp-lit" look).
  if (mat.useDiffuseDetail == 1) {
      vec4 dd = SAMPLE(sampler2D(mat.detailDiffuseMap), uv * mat.detailTiling.x,
                     st1 * mat.detailTiling.x, st2 * mat.detailTiling.x) * mat.detailStr.x;
      albedo *= dd.r;
      smoothness = blendOverlay(smoothness, dd.g);
      metallic = clamp(blendOverlay(metallic, dd.b), 0.0, 1.0);
  }
  // X4's paint and dirt layers, as the shipped complex_surface G-buffer shader
  // runs them (X4_Station_FRONT.rdc, decompiled draw 17317; the plain .glsl in the
  // cats differs). Paint replaces the tinted diffuse by its alpha and pulls metal
  // and smoothness to 0.15x: X4's painted and grimy panels are rough dielectrics.
  // Dirt multiplies in by the vertex dirt amount and only roughens.
  // p2_complex_surface and p2_complex_boron (shipped decompiles: X4_SHOWROOM draw 44243,
  // Boron dock draw 21374) run another pair, dirt first: DATA1.w^2.2 dirt, masked by the
  // low of the height map (p2 surface without one: the dark of the base diffuse),
  // REPLACES the colour and zeroes metal/smoothness (dirtstr unused); paint scales them by
  // 1 - its alpha.
  if (p2Layers) {
      if (mat.useDirt == 1) {
          vec4 dirt = SAMPLE(sampler2D(mat.dirtMap), uv * mat.hullLayerStr.z,
                           st1 * mat.hullLayerStr.z, st2 * mat.hullLayerStr.z);
          float mask = mat.useHeightMap == 1 || mat.hullLayers == 2 ? 1.0 - height : 1.0 - texel.r;
          float f = 1.0 - pow(clamp(s.data.w, 0.0, 1.0), 2.2) * mask * dirt.a;
          albedo = mix(dirt.rgb, albedo, f);
          metallic *= f;
          smoothness *= f;
      }
      if (mat.usePaint == 1) {
          float strength = clamp(mat.hullLayerStr.x, 0.0, 1.0);
          vec4 paint = SAMPLE(sampler2D(mat.paintMap), s.uv2, s.duv2dx, s.duv2dy) * strength;
          float k = strength * paint.a;
          albedo = mix(albedo, paint.rgb, k);
          metallic *= 1.0 - k;
          smoothness *= 1.0 - k;
      }
  } else {
      if (mat.usePaint == 1) {
          vec4 paint = SAMPLE(sampler2D(mat.paintMap), s.uv2, s.duv2dx, s.duv2dy) * clamp(mat.hullLayerStr.x, 0.0, 1.0);
          float a = clamp(paint.a, 0.0, 1.0);
          albedo = mix(albedo, paint.rgb, a);
          float k = mix(1.0, 0.15, clamp(2.0 * a, 0.0, 1.0));
          metallic *= k;
          smoothness *= k;
      }
      // p1_multimat runs X4's plain-source dirt instead: UV map 1, amount = COLOR0.a x 2
      // (common.vert.glsl, 128 = 1.0); p2_multimat: UV0, amount = DATA1.w (in COLOR0.a).
      // The decals run this pair too (complex_projection*.frag.glsl; p2's matches its
      // shipped build, X4_SHOWROOM draw 45993), p2 with the DATA1.w amount.
      if (mat.useDirt == 1) {
          bool uv1Dirt = mat.useMultimat == 1;
          vec2 dirtUv = uv1Dirt ? s.uv1 : uv;
          vec2 dirtDx = (uv1Dirt ? s.duv1dx : st1) * mat.hullLayerStr.z;
          vec2 dirtDy = (uv1Dirt ? s.duv1dy : st2) * mat.hullLayerStr.z;
          vec4 dirt = SAMPLE(sampler2D(mat.dirtMap), dirtUv * mat.hullLayerStr.z, dirtDx, dirtDy);
          float amount = mat.hullLayers == 4 ? s.data.w : s.data.z;
          float f = clamp(amount * clamp(mat.hullLayerStr.y, 0.0, 1.0), 0.0, 1.0);
          if (mat.useMultimat == 1) f = clamp(s.color.a * 2.0 * mat.hullLayerStr.y, 0.0, 1.0);
          if (mat.useMultimat == 2) f = clamp(s.color.a * mat.hullLayerStr.y, 0.0, 1.0);
          albedo = mix(albedo, albedo * dirt.rgb, f);
          smoothness -= smoothness * (1.0 - dirt.a) * f;
      }
  }
  // Boron's iridescence: the albedo hue-rotated by -54 degrees toward grazing angles.
  if (mat.hullLayers == 2) {
      float w = 1.0 - clamp(pow(abs(dot(N, V)), 0.6), 0.0, 1.0);
      albedo = mix(albedo, clamp(hueRotate(albedo, radians(-54.0)), 0.0, 1.0), w);
  }
  float roughness = clamp(1.0 - smoothness, 0.01, 1.0);

  vec3 emissive = vec3(0.0);
  if (mat.useEmissiveMap == 1) {
      // Decoded here, not by an sRGB upload: the faction hue math below needs the
      // encoded values.
      // X4's glow chain (complex_surface_pom.frag.glsl): alpha boosts the mask
      // 1-10x, then color_glowStr scales the result.
      // The decal shaders skip the alpha boost.
      vec4 glow = SAMPLE(sampler2D(mat.emissiveMap), uv, st1, st2);
      float boost = 1.0 + glow.a * (mat.hullLayers >= 3 ? 0.0 : 9.0);
      vec3 detail = vec3(1.0);
      if (mat.glowDetail != 0) {
          detail = SAMPLE(sampler2D(mat.glowDetailMap), uv * mat.glowDetailUv.xy - glowTime * mat.glowDetailUv.zw,
                          st1 * mat.glowDetailUv.xy, st2 * mat.glowDetailUv.xy).rgb * mat.glowDetailMix.x;
      }
      if (mat.glowDetail == 2) {
          // complex_boron_pom_animglow (shipped decompile, Boron dock draw 21456): the layers
          // cut into the decoded glow face-on, a dim base glow takes over toward grazing, and
          // hue/saturation run on decoded values; the alpha boost comes last.
          vec3 rgb = pow(glow.rgb, vec3(2.2));
          vec3 cut = detail + SAMPLE(sampler2D(mat.glowDetail2Map), uv * mat.glowDetail2Uv.xy -
                                     glowTime * mat.glowDetail2Uv.zw, st1 * mat.glowDetail2Uv.xy,
                                     st2 * mat.glowDetail2Uv.xy).rgb * mat.glowDetailMix.y;
          vec3 layered = max(rgb - cut, 0.0);
          float edge = 1.0 - pow(abs(dot(N, V)), 0.5 / max(1.0 - mat.glowDetailMix.w, 1e-4));
          rgb = mix(layered, rgb * mat.glowDetailMix.z, clamp(edge, 0.0, 1.0));
          rgb = mix(vec3(dot(rgb, vec3(0.3086, 0.6094, 0.0820))), rgb, mat.glowAdjust.y);
          rgb = hueRotate(rgb, (0.5 - mat.glowAdjust.x) * 6.28318530718);
          emissive = clamp(rgb, 0.0, 1.0) * mat.glowStr * boost;
      } else {
          emissive = pow(glow.rgb, vec3(2.2)) * mat.glowStr * boost;
          if (mat.glowAdjust.x != 0.5 || mat.glowAdjust.y != 1.0) {
              // Faction hue/saturation: boost/rotate/clamp encoded RGB, then decode
              // (the accepted look). Moving the matrix after pow() changes colors.
              vec3 rgb = glow.rgb * boost;
              rgb = mix(vec3(dot(rgb, vec3(0.3086, 0.6094, 0.0820))), rgb, mat.glowAdjust.y);
              rgb = hueRotate(rgb, (0.5 - mat.glowAdjust.x) * 6.28318530718);
              emissive = pow(clamp(rgb, 0.0, 1.0), vec3(2.2)) * mat.glowStr;
          }
          // p2_complex_surface/boron: a scrolling layer scales the finished glow.
          emissive *= detail;
      }
  }

  // X4's sun term (global_lights + EvalBRDF, common_frag/lighting_common.glsl):
  // GGX Cook-Torrance, and the sun is a 10-unit sphere at 1000 - the
  // representative-point direction widens the lobe to the sun disc and the
  // sphere normalization damps mirror-smooth hulls, so a flat panel reflecting
  // the sun is a finite glint, not a delta. Everything is scaled by n_dot_l,
  // which is what keeps sun-averted faces dark. No 1/PI on the diffuse: X4 commented it
  // out and V_lightcolor1 is calibrated against that.
  const float PI = 3.14159265;
  vec3 cspec = mix(vec3(0.04), albedo, metallic);
  vec3 cdiff = albedo * (1.0 - metallic)
             * clamp(1.0 - dot(vec3(0.299, 0.587, 0.114), cspec), 0.0, 1.0);
  vec3 Lsun = normalize(sunDirection);
  float ndl = max(dot(N, Lsun), 0.0);
  // x2: the shipped deferred global pass is built with LOCALSPEC (station.rdc,
  // decompiled l_pass_globallight: lcolors = V_lightcolor1 * scale * 2.0), not
  // the plain branch the .glsl source reads as.
  float sunVis = sunShadow(s.position, ndl);
  vec3 sunE = 2.0 * sunColor * sunIntensity * ndl * sunVis;
  if (lightMask == 16) {  // --viewshot shadow term: visibility, distance (10 km), n.l
    return vec4(sunVis, length(s.position - cameraPos) / 10000.0, ndl, 1.0);
  }
  // --viewshot material terms, comparable to X4's G-buffer RT1 / RT3.
  if (lightMask == 32) {
    return vec4(albedo, 1.0);
  }
  if (lightMask == 64) {
    return vec4(roughness, metallic, 0.0, 1.0);
  }

  vec3 R = reflect(-V, N);
  vec3 sunCenter = Lsun * 1000.0;
  vec3 centerToRay = dot(sunCenter, R) * R - sunCenter;
  vec3 L = normalize(sunCenter + centerToRay
           * clamp(10.0 / sqrt(max(0.0001, dot(centerToRay, centerToRay))), 0.0, 1.0));
  // Geometric specular AA (Tokuyoshi & Kaplanyan 2019): widen the GGX lobe by the
  // pixel-footprint variance of N. Thin ribs and curved greebles otherwise sparkle
  // once a highlight is narrower than a sample (the zoomed-out "grit"); using the
  // final normal-mapped N covers normal-map aliasing too. sigma^2 0.25, kappa 0.18.
  vec3 dNx = NORMAL_DX(N);
  vec3 dNy = NORMAL_DY(N);
  float nVar = 0.25 * (dot(dNx, dNx) + dot(dNy, dNy));
  float m = roughness * roughness;
  m = sqrt(clamp(m * m + min(2.0 * nVar, 0.18), 0.0, 1.0));
  float sphereNorm = 1.0 / (1.0 + 0.01 / (PI * max(0.001, m)));

  vec3 H = normalize(V + L);
  float nl = clamp(dot(N, L), 0.0, 1.0);
  float nv = clamp(abs(dot(N, V)) + 1e-8, 0.0, 1.0);
  float nh = clamp(dot(N, H), 0.0, 1.0);
  float vh = clamp(dot(V, H), 0.0, 1.0);
  float a2 = m * m;
  float dd = (nh * a2 - nh) * nh + 1.0;
  float D = a2 / (PI * dd * dd);
  float Vis = 0.5 / max(1e-8, nl * (nv * (1.0 - m) + m) + nv * (nl * (1.0 - m) + m));
  float fr = pow(1.0 - vh, 5.0);
  vec3 F = fr + (1.0 - fr) * cspec;
  vec3 specular = D * Vis * F * sunE * sphereNorm;
  if ((lightMask & 2) == 0) specular = vec3(0.0);

  vec3 diffuse = cdiff * sunE;
  if ((lightMask & 1) == 0) diffuse = vec3(0.0);

  // X4's ambient block of global_lights, unscaled (station.rdc): the env cube
  // specular at lod = maxLod * ambRoughness (X4 dropped the sqrt) plus the
  // cluster ambient, and cdiff x (irradiance + ambient).
  float NdotV = max(dot(N, V), 0.0);
  // Narrower cone at grazing angles: X4's fresnel contour trick.
  float ambRough = mix(roughness * 0.3, roughness, pow(NdotV, 1.0 / 3.0));
  vec3 envSpec = textureLod(envMap, R, envMaxLod * ambRough).rgb + ambientLight;
  // Analytic split-sum env BRDF (Karis) — no LUT texture.
  vec4 r4 = ambRough * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
  float a004 = min(r4.x * r4.x, exp2(-9.28 * NdotV)) * r4.x + r4.y;
  vec2 envAB = vec2(-1.04, 1.04) * a004 + r4.zw;
  vec3 irradiance = textureLod(envMap, N, envMaxLod).rgb + ambientLight;
  vec3 ambient = cdiff * irradiance + envSpec * (cspec * envAB.x + envAB.y);
  if ((lightMask & 4) == 0) ambient = vec3(0.0);

  // X4's global_lights: the occlusion scales the sun diffuse and the ambient,
  // then the whole global sum once more; emissive never.
  float ao = useAo == 1 ? texture(aoMap, s.pixelCoord / aoScreen).r : 1.0;
  if (lightMask == 128) {  // --viewshot SSAO term, comparable to X4's R8 occlusion target
    return vec4(vec3(ao), 1.0);
  }
  vec3 global = min(ambient * ao + diffuse * ao + specular, vec3(10.0)) * ao;

  vec3 local = vec3(0.0);
  if (hasLocalLights == 1 && (lightMask & 8) != 0)
    local = localLights(s.position, N, V, cspec, cdiff, m, ao, s.pixelCoord);
  // Linear HDR out: the tonemap and sRGB encode live in the FramePipeline
  // composite (bloom needs the raw energy; spec 2026-07-17).
  // X4 clamps the lit sum to 10 before exposure (its 'safety clamp'); the sun
  // glint on a mirror panel relies on it.
  vec3 color = min(global + local, vec3(10.0)) + emissive;

  // X4: Alpha = diffuse.a * F_alphascale; vertex alpha is the dirt amount, not opacity.
  float outA = mat.opaquePass == 1 ? 1.0 : drawTint.a * texel.a;
  return vec4(color, outA);
}
)";

// The forward stage: the interpolated surface with hardware derivatives.
constexpr const char* kFlatFsMain = R"(
out vec4 finalColor;
void main() {
  mat = materials[materialId];
  glowTime = animStill != 0 ? 0.0 : animTime;
  // Written on every path: a partially assigned gl_SampleMask is undefined.
  coverage = -1;
  if (lodFade < 1.0) {
    coverage = lodCoverage();
    if (coverage == 0) discard;  // before any derivative use
  }
  gl_SampleMask[0] = coverage;
  Surface s;
  s.position = fragPosition;
  s.normal = fragNormal;
  s.uv0 = fragTexCoord;
  s.uv1 = fragDirtUv;
  s.uv2 = fragPaintUv;
  s.color = fragColor;
  s.data = fragDamageDetail;
  s.dPdx = dFdx(fragPosition);
  s.dPdy = dFdy(fragPosition);
  s.duv0dx = dFdx(fragTexCoord);
  s.duv0dy = dFdy(fragTexCoord);
  s.duv1dx = dFdx(fragDirtUv);
  s.duv1dy = dFdy(fragDirtUv);
  s.duv2dx = dFdx(fragPaintUv);
  s.duv2dy = dFdy(fragPaintUv);
  s.pixelCoord = gl_FragCoord.xy;
  finalColor = shadeSurface(s);
}
)";

// The placed parts and their texture scroll, read by the vertex stages and the resolve.
constexpr const char* kInstanceGlsl = R"(
// One world matrix per placed part; each draw's visible list picks its instances.
layout(std430, binding = 0) readonly buffer Instances { mat4 instances[]; };
layout(std430, binding = 1) readonly buffer Visible { uint visible[]; };
// Texture-scroll transforms (C++ UvTransforms) per animated part; slot 0 = identity.
struct UvAnim {
  vec4 linear[4];
  vec2 offset[4];
};
layout(std430, binding = 6) readonly buffer UvAnims { UvAnim uvAnims[]; };
)";

// Shared head of the station vertex stages: the arena's attributes (GeometryArena pins
// raylib's locations) and the instance fetch. `mvp` carries only camera*flip, so
// mvp*instanceTransform() reproduces flip*world*local with no per-instance baking.
constexpr const char* kStationVsHead = R"(#version 430
#extension GL_ARB_shader_draw_parameters : require
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;
layout(location = 5) in vec2 vertexTexCoord2;  // UV map 1, p1_multimat's dirt UV
layout(location = 8) in vec4 vertexUv23;       // UV maps 2 (xy, X4's paint UV) and 3 (zw)
layout(location = 7) in vec4 vertexData;       // X4 DATA1 (p2_multimat: DATA2) bytes, 0-255
// kInstanceGlsl follows: a macro, so it may name its buffers.
#define instanceTransform() instances[visible[gl_BaseInstanceARB + gl_InstanceID]]
uniform int drawBase;  // this multi-draw's first DrawData
#define drawIndex (drawBase + gl_DrawIDARB)
flat out int vDrawIndex;
)";

constexpr const char* kInstancedVsMain = R"(
uniform mat4 mvp;
centroid out vec2 fragTexCoord;
centroid out vec2 fragDirtUv;
centroid out vec2 fragPaintUv;
centroid out vec4 fragColor;
centroid out vec3 fragPosition;
centroid out vec3 fragNormal;
out vec4 fragDamageDetail;
flat out int vInstance;  // the geometry pass's id
// The depth prepass and the lit pass link this stage into different programs
// and must land on the same depth for the lit pass's LEQUAL test.
invariant gl_Position;
void main() {
  vDrawIndex = drawIndex;
  vInstance = gl_InstanceID;
  UvAnim uv = uvAnims[draws[drawIndex].uvSlot];
  fragTexCoord = mat2(uv.linear[0]) * vertexTexCoord + uv.offset[0];
  fragDirtUv = mat2(uv.linear[1]) * vertexTexCoord2 + uv.offset[1];
  fragPaintUv = mat2(uv.linear[2]) * vertexUv23.xy + uv.offset[2];
  fragDamageDetail = vertexData / 255.0;
  fragColor = vertexColor;
  mat4 model = instanceTransform();
  vec4 wp = model*vec4(vertexPosition, 1.0);
  fragPosition = vec3(wp);
  // Rigid rotation composed with the (1,1,-1) flip is orthogonal, so the model
  // matrix is its own inverse-transpose: no separate normal matrix needed.
  fragNormal = mat3(model) * vertexNormal;
  gl_Position = mvp*model*vec4(vertexPosition, 1.0);
}
)";

std::string instancedVsSource() {
  return std::string(kStationVsHead) + kInstanceGlsl + kDrawDataGlsl + kInstancedVsMain;
}

// high/common.vert.glsl with FADING_NEARFARANGLE, as p1_complex_unlit binds it,
// for station parts. Meshes without normals skip the angle fade (hasNormals).
constexpr const char* kComplexUnlitVsMain = R"(
uniform mat4 mvp;
out vec2 IO_uv0;
out vec2 IO_uv1;
out vec2 IO_uv2;
out vec2 IO_uv3;
out vec4 IO_color;
out float IO_Fade;

void main() {
  vDrawIndex = drawIndex;
  UvAnim uv = uvAnims[draws[drawIndex].uvSlot];
  UnlitMaterial m = unlitMaterials[materialId];
  mat4 model = instanceTransform();
  gl_Position = mvp * model * vec4(vertexPosition, 1.0);
  vec3 toVertex = vec3(model * vec4(vertexPosition, 1.0)) - cameraPos;
  IO_uv0 = mat2(uv.linear[0]) * vertexTexCoord + uv.offset[0];
  IO_uv1 = mat2(uv.linear[1]) * vertexTexCoord2 + uv.offset[1];
  IO_uv2 = mat2(uv.linear[2]) * vertexUv23.xy + uv.offset[2];
  IO_uv3 = mat2(uv.linear[3]) * vertexUv23.zw + uv.offset[3];
  IO_color = vec4(pow(vertexColor.rgb + vec3(0.00001), vec3(2.2)), vertexColor.a);
  float angle = 1.0;
  if (hasNormals == 1)
    angle = clamp(pow(abs(dot(normalize(mat3(model) * vertexNormal), normalize(toVertex))), m.angleFade.x) - m.angleFade.y, 0.0, 1.0);
  if (m.lightcone != 0) {
    // The lightcones' FADING_DISTANCE, _ANGLE and _CAMERA/FAR_FADE_RANGE go by view depth,
    // linearly; _alpha has only the near fade.
    float z = gl_Position.w;
    IO_Fade = clamp(1.0 + (z / (m.cameraFade.x + 0.00001) - 1.0) * m.cameraFade.y, 0.0, 1.0);
    if (m.lightcone != 3) {
      float range = max(m.cameraFade.w - m.cameraFade.z, 0.00001);
      IO_Fade *= angle * (1.0 - clamp(z - m.cameraFade.z, 0.0, range) / range);
    }
    return;
  }
  float cameradistance = length(toVertex);
  float fadetonear = smoothstep(m.cameraFade.x, m.cameraFade.y, cameradistance);
  float fadetofar = 1.0 - smoothstep(m.cameraFade.z, m.cameraFade.w, cameradistance);
  IO_Fade = fadetonear * fadetofar * angle;
}
)";

// planet_glsl::kComplexUnlitFsHeader's inputs and X4 uniform names for the station
// path: the body it shares with the planets reads those names, aliased here to this
// draw's UnlitMaterial record. kFrameDataGlsl and kUnlitMaterialGlsl go between
// the head and the aliases.
constexpr const char* kStationUnlitFsHead = R"(#version 430
#extension GL_ARB_shader_ballot : require
#extension GL_ARB_sample_shading : require
#extension GL_ARB_bindless_texture : require
in vec2 IO_uv0;
in vec2 IO_uv1;
in vec2 IO_uv2;
in vec2 IO_uv3;
in vec4 IO_color;
in float IO_Fade;
flat in int vDrawIndex;
#define drawIndex readFirstInvocationARB(vDrawIndex)
out vec4 OUT_RT0;
)";
constexpr const char* kStationUnlitFsAliases = R"(
#define colDiffuse drawTint
layout(binding = 15) uniform sampler2D sceneDepth;  // copied after the opaque pass
#define UM unlitMaterials[materialId]
#define S_diffuse_color UM.diffuseColor
#define S_diffuse_bool (UM.hasDiffuse != 0)
#define S_diffusestr UM.diffuseStr
#define S_diffuse_map sampler2D(UM.diffuseMap)
#define S_diffuse2_bool ((UM.layerMask & 1) != 0)
#define S_diffuse2str UM.layerStr.x
#define S_diffuse2_map sampler2D(UM.layerMaps[0])
#define S_diffuse3_bool ((UM.layerMask & 2) != 0)
#define S_diffuse3str UM.layerStr.y
#define S_diffuse3_map sampler2D(UM.layerMaps[1])
#define S_alpha1_bool ((UM.layerMask & 4) != 0)
#define S_alpha1str UM.layerStr.z
#define S_alpha1_map sampler2D(UM.layerMaps[2])
#define S_alpha2_bool ((UM.layerMask & 8) != 0)
#define S_alpha2str UM.layerStr.w
#define S_alpha2_map sampler2D(UM.layerMaps[3])
#define S_alpha3_bool false
#define S_alpha3str 1.0
#define S_alpha3_map S_diffuse_map
#define U_dblend1 UM.dblend.x
#define U_dblend2 UM.dblend.y
#define B_vertexdata0 (UM.vertexColors != 0)
#define U_color_emissive UM.colorEmissive
#define U_base_brightness_shift UM.grade.x
#define U_base_contrast_shift UM.grade.y
#define U_base_saturation_shift UM.grade.z
#define U_base_hue_shift UM.grade.w
#define U_useralphascale UM.userAlphaScale
#define F_alphascale 1.0
#define forcefield UM.isForcefield
#define opaqueUnlit UM.opaque
#define softRange UM.softRange
#define lightcone UM.lightcone
#define F_time (animStill != 0 ? 0.0 : animTime)
#define STATION_HULL
void lightconeMain();
)";

// high/lightcone(_env, _alpha).frag. Without SHADERFLAG_DONT_DECODE_SRGB X4 samples and
// passes the colour props already decoded, then decodes again (LinearFromSRGB_Deprecated):
// the double decode is X4's. Lightcones only draw blended, so no screen-door.
constexpr const char* kStationLightconeFs = R"(
void lightconeMain() {
  vec4 c = S_diffuse_color;
  if (S_diffuse_bool) c = S_diffusestr * texture(S_diffuse_map, IO_uv0);
  if (lightcone == 2) {
    if (B_vertexdata0) c.rgb *= IO_color.rgb;
    // _env's colour key is hard-coded white: 70% of the way to its grey.
    float x = 1.0 / 3.01;
    float Y = 0.33 * (c.r + c.g + c.b);
    c.rgb = 0.3 * c.rgb + 0.7 * vec3(Y, Y, Y / x * (1.0 - 2.0 * x));
  } else {
    if (B_vertexdata0)
      c.rgb *= IO_color.rgb;
    else if (lightcone == 1)
      c.rgb *= U_color_emissive.rgb;
    c.rgb = pow(c.rgb + vec3(0.00001), vec3(2.2));
  }
  float att = 1.0;
  if (hasSceneDepth == 1)
    att = softAtt(softRange.x, softRange.y,
                  abs(viewDepth(texture(sceneDepth, gl_FragCoord.xy / depthViewport).r) -
                      viewDepth(gl_FragCoord.z)));
  float lod = lodFadeInvert == 1 ? 1.0 - lodFade : lodFade;
  if (lightcone == 3) {
    OUT_RT0 = saturate(vec4(c.rgb * colDiffuse.rgb, c.a * U_useralphascale * IO_Fade * att * lod));
    return;
  }
  vec3 rgb = c.rgb * IO_Fade * att * U_useralphascale;
  if (lightcone == 2) rgb = saturate(rgb) * 0.1;
  OUT_RT0 = vec4(clamp(rgb * colDiffuse.rgb * lod, 0.0, 1000.0), 1.0);
}
)";

// Depth-only fragment stage for the sun shadow pass.
constexpr const char* kDepthFs = R"(#version 330
void main() {}
)";

// The SSAO depth prepass: depth only, but on exactly the samples the lit pass
// covers during an LOD dissolve, so both levels of a fading pair stay in it. With
// WRITE_IDS it is the visibility buffer's geometry pass: it also records which triangle
// of which instance of which sub-draw covers each sample (the id layout the compute
// passes read: x = draw index << 16 | instance in the sub-draw, y = primitive).
constexpr const char* kDepthPrepassFsHead = R"(#version 430
#extension GL_ARB_shader_ballot : require
#extension GL_ARB_sample_shading : require
#extension GL_ARB_bindless_texture : require
flat in int vDrawIndex;
flat in int vInstance;
// The sub-draw's index is uniform across the wave, but a flat input doesn't tell the
// compiler so: without the broadcast every bindless material read is a waterfall loop.
#define drawIndex readFirstInvocationARB(vDrawIndex)
)";
constexpr const char* kDepthPrepassFsMain = R"(
#ifdef WRITE_IDS
layout(location = 0) out uvec2 visId;
#endif
#ifdef CUTOUT
centroid in vec2 fragTexCoord;
in vec4 fragDamageDetail;
#endif
void main() {
  int coverage = -1;
  if (lodFade < 1.0) {
    coverage = lodCoverage();
    if (coverage == 0) discard;
  }
#ifdef CUTOUT
  // The forward cutout's alpha at the unshifted uv: the parallax shift only exists in
  // the shading.
  // TWOSIDED ranges share the cutouts' unculled class without the alpha test.
  Material mat = materials[materialId];
  if (mat.alphaTest == 1) {
    float cut = texture(sampler2D(mat.albedoMap), fragTexCoord).a;
    if (mat.useMultimat == 2)
      cut = mat.useMultimatDiffuse == 1
                ? texture(sampler2DArray(mat.multimatDiffuse), vec3(fragTexCoord, fragDamageDetail.z * 255.0)).a
                : 1.0;
    coverage &= cutoutCoverage(cut, mat.hullLayers == 1 || mat.hullLayers == 2);
    if (coverage == 0) discard;
  }
#endif
  gl_SampleMask[0] = coverage;
#ifdef WRITE_IDS
  visId = uvec2((uint(vDrawIndex) << 16) | uint(vInstance), uint(gl_PrimitiveID));
#endif
}
)";

// The compute passes over the geometry pass's ids (kDepthPrepassFsMain); all ones = no
// station surface (sky). SAMPLES and TILE come first (visComputeDefines).
constexpr const char* kVisComputeHead = R"(
layout(local_size_x = TILE, local_size_y = TILE) in;
layout(binding = 29) uniform usampler2DMS visIds;
layout(std430, binding = 10) buffer Tiles {
  uint tileGroups, tileGroupsY, tileGroupsZ;  // the resolve's indirect dispatch
  uint tileCapacity;
  uvec2 tileRecords[];  // (tile, material)
};
uniform ivec2 screenSize;
const uint kNoSurface = 0xFFFFFFFFu;
int drawIndex;
)";

// One work group per tile: the set of lit materials its samples show, one record each.
constexpr const char* kClassifyMain = R"(
shared uint seen[32];  // VisibilityBuffer::kMaxMaterials bits
void main() {
  uint li = gl_LocalInvocationIndex;
  if (li < 32u) seen[li] = 0u;
  barrier();
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (all(lessThan(p, screenSize))) {
    for (int i = 0; i < SAMPLES; ++i) {
      uint id = texelFetch(visIds, p, i).x;
      if (id == kNoSurface) continue;
      uint m = draws[id >> 16].material;
      atomicOr(seen[m >> 5], 1u << (m & 31u));
    }
  }
  barrier();
  if (li < 32u) {
    uint tile = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    for (uint bits = seen[li]; bits != 0u; bits &= bits - 1u) {
      uint slot = atomicAdd(tileGroups, 1u);
      if (slot < tileCapacity)
        tileRecords[slot] = uvec2(tile, li * 32u + uint(findLSB(bits)));
      else
        atomicAdd(tileGroups, 0xFFFFFFFFu);  // the indirect dispatch must stay within capacity
    }
  }
}
)";

// shadeSurface's hooks without hardware derivatives: explicit-gradient fetches, and the
// normal's pixel footprint from the interpolated vertex normal (the normal map's share
// is left out). No cutout: the geometry pass ran it.
constexpr const char* kResolveHooks = R"(
#define SAMPLE(s, uv, dx, dy) textureGrad(s, uv, dx, dy)
#define NORMAL_DX(n) normalDx
#define NORMAL_DY(n) normalDy
vec3 normalDx;
vec3 normalDy;
)";

constexpr const char* kResolveMain = R"(
struct DrawCommand {
  uint count, instanceCount, firstIndex;
  int baseVertex;
  uint baseInstance;
};
layout(std430, binding = 7) readonly buffer Commands { DrawCommand commands[]; };
// ArenaVertex as 14 words; its offsets are pinned in xmf_model.hpp.
layout(std430, binding = 8) readonly buffer Vertices { uint vertexWords[]; };
layout(std430, binding = 9) readonly buffer Indices { uint indices[]; };
layout(rgba16f, binding = 0) uniform restrict writeonly image2DMS shadedColor;
uniform mat4 mvp;
uniform vec2 samplePos[SAMPLES];

// One triangle corner as the vertex stage outputs it (kInstancedVsMain).
struct Corner {
  vec3 position;
  vec3 normal;
  vec2 uv0, uv1, uv2;
  vec4 color, data;
};

vec2 wordsToVec2(uint b) { return uintBitsToFloat(uvec2(vertexWords[b], vertexWords[b + 1u])); }

Corner fetchCorner(uint vertex, mat4 model, UvAnim anim) {
  uint b = vertex * 14u;
  Corner c;
  vec3 p = uintBitsToFloat(uvec3(vertexWords[b], vertexWords[b + 1u], vertexWords[b + 2u]));
  int packedN = int(vertexWords[b + 3u]);  // INT_2_10_10_10_REV snorm
  vec3 n = max(vec3(ivec3(packedN << 22, packedN << 12, packedN << 2) >> 22) / 511.0, -1.0);
  c.position = vec3(model * vec4(p, 1.0));
  c.normal = mat3(model) * n;
  c.uv0 = mat2(anim.linear[0]) * wordsToVec2(b + 4u) + anim.offset[0];
  c.uv1 = mat2(anim.linear[1]) * wordsToVec2(b + 6u) + anim.offset[1];
  c.uv2 = mat2(anim.linear[2]) * wordsToVec2(b + 8u) + anim.offset[2];
  c.color = unpackUnorm4x8(vertexWords[b + 12u]);
  c.data = unpackUnorm4x8(vertexWords[b + 13u]);
  return c;
}

// Perspective-correct barycentrics at `ndc` and their change one pixel right and up
// (Schied & Dachsbacher's analytic derivatives, as The Forge's visibility buffer).
struct Barycentrics {
  vec3 l, ddx, ddy;
};
Barycentrics barycentrics(vec4 c0, vec4 c1, vec4 c2, vec2 ndc, vec2 ndcPerPixel) {
  vec3 invW = 1.0 / vec3(c0.w, c1.w, c2.w);
  vec2 n0 = c0.xy * invW.x;
  vec2 n1 = c1.xy * invW.y;
  vec2 n2 = c2.xy * invW.z;
  float invDet = 1.0 / determinant(mat2(n2 - n1, n0 - n1));
  vec3 ddx = vec3(n1.y - n2.y, n2.y - n0.y, n0.y - n1.y) * invDet * invW;
  vec3 ddy = vec3(n2.x - n1.x, n0.x - n2.x, n1.x - n0.x) * invDet * invW;
  float ddxSum = dot(ddx, vec3(1.0));
  float ddySum = dot(ddy, vec3(1.0));
  vec2 d = ndc - n0;
  float interpInvW = invW.x + d.x * ddxSum + d.y * ddySum;
  vec3 l = vec3(invW.x + d.x * ddx.x + d.y * ddy.x, d.x * ddx.y + d.y * ddy.y,
                d.x * ddx.z + d.y * ddy.z) / interpInvW;
  ddx *= ndcPerPixel.x;
  ddy *= ndcPerPixel.y;
  ddxSum *= ndcPerPixel.x;
  ddySum *= ndcPerPixel.y;
  Barycentrics b;
  b.l = l;
  b.ddx = (l * interpInvW + ddx) / (interpInvW + ddxSum) - l;
  b.ddy = (l * interpInvW + ddy) / (interpInvW + ddySum) - l;
  return b;
}

// Relative to corner 0: the weights sum to 1 (the steps to 0) only up to rounding, so the
// absolute form leaves a ~1e-7 x |value| gradient on a constant attribute. A coarse LOD with
// collapsed UVs then gets a random per-pixel tangent from normalize(noise) (the dotted
// patch on the antimatter converter during its dissolve); the forward dFdx is exactly 0.
#define LERP(f) (c0.f + b.l.y * (c1.f - c0.f) + b.l.z * (c2.f - c0.f))
#define LERP_DX(f) (b.ddx.y * (c1.f - c0.f) + b.ddx.z * (c2.f - c0.f))
#define LERP_DY(f) (b.ddy.y * (c1.f - c0.f) + b.ddy.z * (c2.f - c0.f))

vec3 normalStep(vec3 n, vec3 dn) {
  return dot(n, n) > 1e-12 ? normalize(n + dn) - normalize(n) : vec3(0.0);
}

// Shades the surface of `primitive` of `instance` in sub-draw drawIndex at window
// position `point`.
vec4 shadeTriangle(uint instance, uint primitive, vec2 point, vec2 pixelCentre) {
  DrawCommand cmd = commands[drawIndex];
  mat4 model = instances[visible[cmd.baseInstance + instance]];
  UvAnim anim = uvAnims[draws[drawIndex].uvSlot];
  uint first = cmd.firstIndex + primitive * 3u;
  Corner c0 = fetchCorner(uint(int(indices[first]) + cmd.baseVertex), model, anim);
  Corner c1 = fetchCorner(uint(int(indices[first + 1u]) + cmd.baseVertex), model, anim);
  Corner c2 = fetchCorner(uint(int(indices[first + 2u]) + cmd.baseVertex), model, anim);
  vec2 ndcPerPixel = 2.0 / vec2(screenSize);
  Barycentrics b = barycentrics(mvp * vec4(c0.position, 1.0), mvp * vec4(c1.position, 1.0),
                                mvp * vec4(c2.position, 1.0), point * ndcPerPixel - 1.0,
                                ndcPerPixel);
  Surface s;
  s.position = LERP(position);
  s.normal = LERP(normal);
  s.uv0 = LERP(uv0);
  s.uv1 = LERP(uv1);
  s.uv2 = LERP(uv2);
  // Unorm attributes stay in range: an edge sample can sit a rounding error outside the
  // triangle, and an extrapolated colour below 0 turns its sRGB decode into NaN.
  s.color = clamp(LERP(color), 0.0, 1.0);
  s.data = clamp(LERP(data), 0.0, 1.0);
  s.dPdx = LERP_DX(position);
  s.dPdy = LERP_DY(position);
  s.duv0dx = LERP_DX(uv0);
  s.duv0dy = LERP_DY(uv0);
  s.duv1dx = LERP_DX(uv1);
  s.duv1dy = LERP_DY(uv1);
  s.duv2dx = LERP_DX(uv2);
  s.duv2dy = LERP_DY(uv2);
  s.pixelCoord = pixelCentre;
  bool smoothNormals = hasNormals == 1;
  normalDx = smoothNormals ? normalStep(s.normal, LERP_DX(normal)) : vec3(0.0);
  normalDy = smoothNormals ? normalStep(s.normal, LERP_DY(normal)) : vec3(0.0);
  glowTime = animStill != 0 ? 0.0 : animTime;
  return shadeSurface(s);
}

// One work group per (tile, material) record: the material is the same for the whole
// group, so its bindless handles are dynamically uniform.
void main() {
  if (gl_WorkGroupID.x >= tileCapacity) return;
  uvec2 record = tileRecords[gl_WorkGroupID.x];
  mat = materials[record.y];
  uint tilesX = (uint(screenSize.x) + uint(TILE) - 1u) / uint(TILE);
  ivec2 p = ivec2(record.x % tilesX, record.x / tilesX) * TILE + ivec2(gl_LocalInvocationID.xy);
  if (any(greaterThanEqual(p, screenSize))) return;
  uvec2 ids[SAMPLES];
  for (int i = 0; i < SAMPLES; ++i) ids[i] = texelFetch(visIds, p, i).xy;
  uint done = 0u;
  for (int i = 0; i < SAMPLES; ++i) {
    if ((done & (1u << i)) != 0u || ids[i].x == kNoSurface) continue;
    drawIndex = int(ids[i].x >> 16);
    if (draws[drawIndex].material != record.y) continue;
    uint group = 0u;
    vec2 at = vec2(0.0);
    float n = 0.0;
    for (int j = i; j < SAMPLES; ++j) {
      if (ids[j] != ids[i]) continue;
      group |= 1u << j;
      at += samplePos[j];
      n += 1.0;
    }
    done |= group;
    // A fully covered pixel shades at its centre, like the forward pass; an edge at the
    // mean of its covered samples, which lies inside the triangle (centroid sampling).
    vec2 point = vec2(p) + (n == float(SAMPLES) ? vec2(0.5) : at / n);
    vec4 color = shadeTriangle(ids[i].x & 0xFFFFu, ids[i].y, point, vec2(p) + 0.5);
    for (int j = 0; j < SAMPLES; ++j)
      if ((group & (1u << j)) != 0u) imageStore(shadedColor, p, j, color);
  }
}
)";

std::string visComputeDefines() {
  return "#version 430\n#extension GL_ARB_bindless_texture : require\n#define SAMPLES " +
         std::to_string(VisibilityBuffer::kSamples) + "\n#define TILE " +
         std::to_string(VisibilityBuffer::kTile) + "\n";
}

std::string classifySource() {
  return visComputeDefines() + kVisComputeHead + kDrawDataGlsl + kClassifyMain;
}

std::string resolveSource() {
  return visComputeDefines() + kVisComputeHead + kResolveHooks + kFrameDataGlsl + kDrawDataGlsl +
         kMaterialGlsl + kInstanceGlsl + kFlatFsBody + kShadeSurfaceGlsl + kResolveMain;
}

std::string flatFsSource() {
  return std::string(kFlatFsHead) + kFrameDataGlsl + kDrawDataGlsl + kMaterialGlsl + kFlatFsBody +
         kLodCoverageGlsl + kCutoutGlsl + kShadeSurfaceGlsl + kFlatFsMain;
}

HullLayers hullLayersOf(const MaterialDef& material) {
  if (usesHeightMask(material))
    return isBoron(material) ? HullLayers::Boron : HullLayers::P2Surface;
  if (material.shader == "p1_complex_projection") return HullLayers::P1Projection;
  if (material.shader == "p2_complex_projection") return HullLayers::P2Projection;
  return HullLayers::P1;
}

BlendClass classifyBlend(const std::string& raw) {
  if (raw == "ALPHA1" || raw == "ALPHA1NOG") return BlendClass::Mask;
  if (raw == "ADDITIVE" || raw == "ADDITIVE_SINGLE" || raw == "SCREEN") return BlendClass::Additive;
  if (raw.rfind("ALPHA8", 0) == 0 || raw == "PREALPHA8") return BlendClass::AlphaBlend;
  if (raw == "DISTORTION") return BlendClass::Skip;
  return BlendClass::Opaque;  // NONE / "" / unknown
}

Mat4 toMat4(const ::Matrix& m) {
  const ::float16 f = MatrixToFloatV(m);
  Mat4 out{};
  std::copy(std::begin(f.v), std::end(f.v), out.begin());
  return out;
}

// Respecified on every write (orphaned), so draws still reading the old contents
// keep them instead of stalling the upload.
void uploadBuffer(GLenum target, unsigned int& buffer, const void* data, std::size_t bytes,
                  GLenum usage = GL_DYNAMIC_DRAW) {
  if (buffer == 0) glGenBuffers(1, &buffer);
  glBindBuffer(target, buffer);
  // Never empty: a zero-size buffer cannot be bound to a block.
  glBufferData(target, static_cast<GLsizeiptr>(std::max<std::size_t>(bytes, 16)), nullptr, usage);
  glBufferSubData(target, 0, static_cast<GLsizeiptr>(bytes), data);
  glBindBuffer(target, 0);
}

}  // namespace

MeshCache::MeshCache(const NativeAssets& native) {
  std::unordered_map<std::string, NativePart> parts;
  for (const auto& [id, module] : native.catalog.all()) {
    for (const auto& ref : module.meshRefs) {
      if (ref.xmfPath.empty()) continue;
      parts[ref.meshKey] = {ref.xmfPath, ref.materialRef, ref.effect};
      transientSlotSize_ = std::max(transientSlotSize_, module.meshRefs.size());
      const bool animated = !ref.uvAnimation.channels.empty();
      if (animated) uvAnimations_[ref.meshKey] = ref.uvAnimation;
      const auto suffix = ref.xmfPath.rfind("-lod0.xmf");
      if (suffix == std::string::npos || suffix + 9 != ref.xmfPath.size()) continue;
      for (int lod = 1; lod < 4; ++lod) {
        const std::string key = meshKeyLod(ref.meshKey, lod);
        parts[key] = {ref.xmfPath.substr(0, suffix) + "-lod" + std::to_string(lod) + ".xmf",
                      ref.materialRef, ref.effect};
        if (animated) uvAnimations_[key] = ref.uvAnimation;
      }
    }
  }
  streamer_ = std::make_unique<NativeStreamer>(
      [archive = native.archive](const std::string& path) {
        return archiveAssetReader(*archive)(path);
      },
      native.materials, std::move(parts));

  // GL context is live by contract (constructed after InitWindow), so compile here.
  gl_ = loadGlExtensions();
  const std::string instancedVs = instancedVsSource();
  instancedShader_ = LoadShaderFromMemory(instancedVs.c_str(), flatFsSource().c_str());
  depthInstancedShader_ = LoadShaderFromMemory(instancedVs.c_str(), kDepthFs);
  depthPrepassShader_ = LoadShaderFromMemory(
      instancedVs.c_str(),
      (std::string(kDepthPrepassFsHead) + kDrawDataGlsl + kLodCoverageGlsl + kDepthPrepassFsMain)
          .c_str());
  visibilityShader_ = LoadShaderFromMemory(
      instancedVs.c_str(), (std::string(kDepthPrepassFsHead) + "#define WRITE_IDS\n" +
                            kDrawDataGlsl + kLodCoverageGlsl + kDepthPrepassFsMain)
                               .c_str());
  visibilityMaskShader_ = LoadShaderFromMemory(
      instancedVs.c_str(),
      (std::string(kDepthPrepassFsHead) + "#define WRITE_IDS\n#define CUTOUT\n" + kDrawDataGlsl +
       kMaterialGlsl + kLodCoverageGlsl + kCutoutGlsl + kDepthPrepassFsMain)
          .c_str());
  const std::string complexUnlitFs = std::string(kStationUnlitFsHead) + kFrameDataGlsl +
                                     kDrawDataGlsl + kUnlitMaterialGlsl + kStationUnlitFsAliases +
                                     planet_glsl::kSharedFs + planet_glsl::kComplexUnlitFsMain +
                                     kStationLightconeFs;
  complexUnlitShader_ =
      LoadShaderFromMemory((std::string(kStationVsHead) + kInstanceGlsl + kDrawDataGlsl +
                            kFrameDataGlsl + kUnlitMaterialGlsl + kComplexUnlitVsMain)
                               .c_str(),
                           complexUnlitFs.c_str());
  // A failed compile hands back raylib's default shader.
  shadersOk_ = gl_.missing.empty();
  for (const ::Shader* sh : {&instancedShader_, &complexUnlitShader_, &depthInstancedShader_,
                             &depthPrepassShader_, &visibilityShader_, &visibilityMaskShader_})
    shadersOk_ = shadersOk_ && sh->id != 0 && sh->id != rlGetShaderIdDefault();
  if (!shadersOk_) TraceLog(LOG_ERROR, "MeshCache: a station shader failed; modules draw as boxes");
  // X4SB_FORWARD=1 keeps the forward opaque path, the A/B reference for the visibility buffer.
  const char* forward = std::getenv("X4SB_FORWARD");
  if (forward == nullptr || std::string_view(forward) != "1")
    visibility_ = std::make_unique<VisibilityBuffer>(classifySource(), resolveSource());
  // X4SB_NO_OCCLUSION=1 draws every CPU-culled instance, the A/B reference for the cull.
  const char* noOcclusion = std::getenv("X4SB_NO_OCCLUSION");
  if (noOcclusion == nullptr || std::string_view(noOcclusion) != "1") {
    occlusion_ = std::make_unique<OcclusionCull>();
    if (!occlusion_->ok()) occlusion_.reset();
  }
  const char* occStats = std::getenv("X4SB_OCCLUSION_STATS");
  occlusionStatsOn_ = occStats != nullptr && std::string_view(occStats) == "1";

  // kFlatFsBody's samplerCube must never sample an unbound unit (undefined per GL
  // spec). Harness paths without a Sky keep this black cube = dark ambient.
  fallbackEnv_ = makeFallbackCubemap();
  setEnvMap(fallbackEnv_, 0.0f);
  const std::array<unsigned char, 4> white{255, 255, 255, 255};
  glGenTextures(1, &fallbackArray_);
  glBindTexture(GL_TEXTURE_2D_ARRAY, fallbackArray_);
  glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 1);
  glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                  white.data());
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

MeshCache::Statistics MeshCache::statistics() const {
  Statistics result = uploaded_;
  result.uploadedVertexBytes = arena_.vertexBytes();
  result.uploadedIndexBytes = arena_.indexBytes();
  std::unordered_set<std::uint32_t> opaqueMaterials;
  std::unordered_set<std::uint32_t> maskMaterials;
  for (const auto& [key, model] : cache_) {
    if (!model) {
      ++result.missingModels;
      continue;
    }
    ++result.loadedModels;
    for (const Part::Range& range : model->ranges) {
      result.maxRangeIndices = std::max<std::size_t>(result.maxRangeIndices, range.indexCount);
      if (range.blendClass == BlendClass::Additive || range.blendClass == BlendClass::AlphaBlend) {
        ++result.transparentRanges;
      } else if (range.unlit) {
        ++result.unlitOpaqueRanges;
      } else if (range.blendClass == BlendClass::Mask) {
        ++result.litMaskRanges;
        maskMaterials.insert(range.materialId);
      } else if (range.blendClass == BlendClass::Opaque) {
        ++result.litOpaqueRanges;
        opaqueMaterials.insert(range.materialId);
      }
    }
  }
  result.litOpaqueMaterials = opaqueMaterials.size();
  result.litMaskMaterials = maskMaterials.size();
  for (const auto& [key, texture] : texturesByUri_) {
    if (texture.id != 0)
      ++result.loadedTextures;
    else
      ++result.missingTextures;
  }
  for (const auto& [key, texture] : arrayTexturesByUri_) {
    if (texture.id != 0)
      ++result.loadedTextures;
    else
      ++result.missingTextures;
  }
  result.pendingModels = pendingNative_.size();
  result.shadersReady = shadersOk_;
  result.materials = materials_.items().size() + unlitMaterials_.items().size();
  return result;
}

MeshCache::~MeshCache() {
  streamer_.reset();  // join workers before tearing down anything else
  for (const unsigned int buffer :
       {marksSsbo_, instanceSsbo_, visibleSsbo_, materialSsbo_, pomLayerSsbo_, unlitMaterialSsbo_,
        frameUbo_, indirectBuffer_, drawDataSsbo_, uvAnimSsbo_})
    if (buffer != 0) glDeleteBuffers(1, &buffer);
  // Parts share the canonical textures: unload each exactly once.
  for (auto& [uri, tex] : texturesByUri_) UnloadTexture(tex);
  for (const auto& [uri, tex] : arrayTexturesByUri_)
    if (tex.id != 0) rlUnloadTexture(tex.id);
  // UnloadShader skips raylib's default shader, which a failed compile returns.
  for (const ::Shader& sh : {instancedShader_, complexUnlitShader_, depthInstancedShader_,
                             depthPrepassShader_, visibilityShader_, visibilityMaskShader_})
    UnloadShader(sh);
  if (fallbackEnv_ != 0) rlUnloadTexture(fallbackEnv_);
  if (fallbackArray_ != 0) glDeleteTextures(1, &fallbackArray_);
}

void MeshCache::request(const std::string& meshKey, bool optional, bool urgent) {
  if (cache_.count(meshKey) != 0) return;
  if (pendingNative_.emplace(meshKey, optional).second) streamer_->requestModel(meshKey, urgent);
}

void MeshCache::beginFrame() {
  prepassDepthValid_ = false;
  visibilityPass_ = false;
  visibilityResolved_ = false;
  bindShadowMap();
  // ~1/4 of a 60 Hz frame: enough to drain a typical part per frame without
  // turning a big import into a visible hitch.
  pumpStreaming(0.004);
}

void MeshCache::pumpStreaming(double budgetSeconds, std::chrono::milliseconds wait) {
  if (pendingNative_.empty() && texturesToUpload_.empty()) return;
  // Only wait when this pump would otherwise have nothing new: a model parked on
  // in-flight textures must not turn warm()'s loop into a busy spin.
  auto completed = streamer_->takeCompleted();
  if (completed.models.empty() && completed.textures.empty() && texturesToUpload_.empty())
    completed = streamer_->takeCompleted(wait);
  for (auto& texture : completed.textures) texturesToUpload_.push_back(std::move(texture));
  for (auto& model : completed.models) modelsAwaitingTextures_.push_back(std::move(model));

  const double deadline = GetTime() + budgetSeconds;
  bool progressed = false;
  const auto withinBudget = [&] { return !progressed || GetTime() < deadline; };
  while (!texturesToUpload_.empty() && withinBudget()) {
    uploadNativeTexture(std::move(texturesToUpload_.front()));
    texturesToUpload_.pop_front();
    progressed = true;
  }
  const auto texturesResident = [this](const StreamedModel& model) {
    for (const auto& material : model.materials)
      for (const auto& ref : materialTextures(material))
        if ((ref.array ? arrayTexturesByUri_.count("native:" + textureKey(ref))
                       : texturesByUri_.count("native:" + textureKey(ref))) == 0)
          return false;
    return true;
  };
  for (auto it = modelsAwaitingTextures_.begin();
       it != modelsAwaitingTextures_.end() && withinBudget();) {
    if (!texturesResident(*it)) {
      ++it;
      continue;
    }
    finishNative(std::move(*it));
    it = modelsAwaitingTextures_.erase(it);
    progressed = true;
  }
}

void MeshCache::uploadNativeTexture(StreamedTexture&& streamed) {
  const std::string key = "native:" + textureKey(streamed.ref);
  unsigned int id = 0;
  if (streamed.ref.array) {
    const auto texture =
        streamed.dds ? loadDdsArrayFromMemory(*streamed.dds, streamed.ref.srgb) : DdsArrayTexture{};
    id = texture.id;
    arrayTexturesByUri_.emplace(key, texture);
  } else {
    const auto texture =
        streamed.dds ? loadDdsFromMemory(*streamed.dds, streamed.ref.srgb) : ::Texture2D{};
    id = texture.id;
    texturesByUri_.emplace(key, texture);
  }
  if (id == 0) {
    TraceLog(LOG_WARNING, "MeshCache: unable to stream texture %s%s",
             streamed.ref.array ? "array " : "", streamed.ref.path.c_str());
    return;
  }
  uploaded_.textureBytes += streamed.dds->size();
  const auto layout = parseDds(*streamed.dds);
  if (layout && layout->format == DdsFormat::Bc5) bc5Textures_.insert(id);
}

void MeshCache::finishNative(StreamedModel&& streamed) {
  const auto pending = pendingNative_.find(streamed.key);
  const bool optional = pending != pendingNative_.end() && pending->second;
  if (pending != pendingNative_.end()) pendingNative_.erase(pending);
  const auto placement =
      streamed.part ? arena_.append(streamed.part->vertices, streamed.part->indices) : std::nullopt;
  if (!placement) {
    if (!optional && !streamed.effect)
      TraceLog(LOG_WARNING, "MeshCache: unable to stream mesh %s", streamed.key.c_str());
    cache_.emplace(streamed.key, std::nullopt);
    return;
  }
  const PreparedPart& prepared = *streamed.part;
  uploaded_.meshRanges += prepared.ranges.size();
  uploaded_.partVertices += prepared.vertices.size();
  uploaded_.partIndices += prepared.indices.size();
  // Texture ids of the resident uploads; 0 = absent or failed to load.
  const auto textureId = [&](const std::string& path, bool srgb) -> unsigned int {
    if (path.empty()) return 0;
    return texturesByUri_.at("native:" + textureKey({normalizeTexturePath(path), srgb})).id;
  };
  const auto bitmap = [](const MaterialDef& material, const char* name) -> const std::string& {
    static const std::string kNone;
    const auto it = material.properties.bitmaps.find(name);
    return it != material.properties.bitmaps.end() ? it->second : kNone;
  };
  Part model;
  model.hasNormals = prepared.hasNormals;
  model.boundsMin = prepared.boundsMin;
  model.boundsMax = prepared.boundsMax;
  for (std::size_t index = 0; index < prepared.ranges.size(); ++index) {
    const PreparedRange& prepRange = prepared.ranges[index];
    const MaterialDef& material = streamed.materials[index];
    const auto number = [&](const std::string& key, float fallback) {
      const auto it = material.properties.floats.find(key);
      return it != material.properties.floats.end() ? it->second : fallback;
    };
    Part::Range& range = model.ranges.emplace_back();
    range.firstIndex = placement->firstIndex + prepRange.firstIndex;
    range.indexCount = prepRange.indexCount;
    range.baseVertex = static_cast<std::int32_t>(placement->firstVertex + prepRange.baseVertex);
    range.blendClass = streamed.effect && !isPortedFx(material) ? BlendClass::Skip
                                                                : classifyBlend(material.blendMode);
    range.twoSided = material.blendMode == "TWOSIDED";
    const unsigned int diffuse = textureId(material.diffuseMap, true);
    const unsigned int emissive = textureId(material.emissiveMap, false);
    const bool unlit =
        (range.blendClass == BlendClass::Additive &&
         (isComplexUnlit(material) || isSimpleUnlit(material))) ||
        (range.blendClass == BlendClass::Opaque && isSimpleUnlit(material)) ||
        ((range.blendClass == BlendClass::Additive || range.blendClass == BlendClass::AlphaBlend) &&
         isLightcone(material));
    range.unlit = unlit;
    range.screen = unlit && material.blendMode == "SCREEN";
    range.glowing = (material.glowStr > 0.0F && emissive != 0) || unlit ||
                    range.blendClass == BlendClass::Additive;
    if (range.blendClass != BlendClass::Skip) {
      const bool transparent =
          range.blendClass == BlendClass::Additive || range.blendClass == BlendClass::AlphaBlend;
      model.inPass[static_cast<std::size_t>(transparent ? DrawPass::Transparent
                                                        : DrawPass::Opaque)] = true;
    }
    if (unlit) {
      GpuUnlitMaterial m = complexUnlitInfo(material, streamed.vertexColors);
      m.diffuseStr = material.diffuseStr;
      m.hasDiffuse = diffuse != 0 ? 1 : 0;
      m.diffuseMap = handle2D(diffuse);
      m.opaque = range.blendClass == BlendClass::Opaque ? 1 : 0;
      range.materialId = unlitMaterials_.add(m);
      materialsDirty_ = true;
      continue;
    }

    GpuMaterial m;
    const std::uint64_t white = handle2D(0);
    const std::uint64_t whiteArray = handleArray(0);
    m.paintMap = m.dirtMap = m.heightMap = m.glowDetailMap = m.glowDetail2Map = white;
    m.multimatDiffuse = m.multimatNormal = m.multimatHeight = whiteArray;
    m.diffuseStr = material.diffuseStr;
    m.glowStr = material.glowStr;
    m.glowAdjust = {material.glowHue, material.glowSaturation};
    m.alphaTest = range.blendClass == BlendClass::Mask ? 1 : 0;
    m.additiveUnlit = range.blendClass == BlendClass::Additive ? 1 : 0;
    m.opaquePass =
        range.blendClass == BlendClass::Opaque || range.blendClass == BlendClass::Mask ? 1 : 0;
    m.albedoMap = handle2D(diffuse);
    const unsigned int normal = textureId(material.normalMap, false);
    const unsigned int metal = textureId(material.metalMap, false);
    const unsigned int smooth = textureId(material.smoothMap, false);
    const unsigned int detailDiffuse = textureId(material.diffuseDetailMap, true);
    const unsigned int detailNormal = textureId(material.normalDetailMap, false);
    m.normalMap = handle2D(normal);
    m.metalMap = handle2D(metal);
    m.smoothMap = handle2D(smooth);
    m.emissiveMap = handle2D(emissive);
    m.detailDiffuseMap = handle2D(detailDiffuse);
    m.detailNormalMap = handle2D(detailNormal);
    m.useNormalMap = normal != 0 ? 1 : 0;
    m.useMetalMap = metal != 0 ? 1 : 0;
    m.useSmoothMap = smooth != 0 ? 1 : 0;
    m.useEmissiveMap = emissive != 0 ? 1 : 0;
    m.normalTwoChannel = bc5Textures_.count(normal) != 0 ? 1 : 0;
    m.mrFactor = {material.metallness.value_or(metal != 0 ? 1.0F : 0.0F),
                  1.0F - material.smoothness.value_or(smooth != 0 ? 1.0F : 0.0F)};
    // X4's detail layer: normal detail additionally requires the base normal map,
    // matching its nesting (SAMPLE_NORMAL_MAP2).
    m.useDiffuseDetail = detailDiffuse != 0 ? 1 : 0;
    m.useNormalDetail = normal != 0 && detailNormal != 0 ? 1 : 0;
    m.detailTiling = {material.diffuseDetailTiling, material.normalDetailTiling};
    m.detailStr = {material.diffuseDetailStr, material.normalDetailStr};
    if (usesHullLayers(material)) {
      // p1_complex_surface.xml: <depends name="diffuse_paint_map" value="uv2"/>.
      const unsigned int paint =
          streamed.paintUv ? textureId(bitmap(material, kHullLayerMaps[0]), true) : 0;
      const unsigned int dirt = textureId(bitmap(material, kHullLayerMaps[1]), true);
      m.paintMap = handle2D(paint);
      m.dirtMap = handle2D(dirt);
      m.usePaint = paint != 0 ? 1 : 0;
      m.useDirt = dirt != 0 ? 1 : 0;
      m.hullLayerStr = {number("diffuse_paintstr", 1.0F), number("color_dirtstr", 1.0F),
                        number("color_dirt_tiling", 1.0F)};
      m.hullLayers = static_cast<std::int32_t>(hullLayersOf(material));
      if (usesHeightMask(material)) {
        const unsigned int height = textureId(bitmap(material, kHeightMap), false);
        m.heightMap = handle2D(height);
        m.useHeightMap = height != 0 ? 1 : 0;
        // The boron build hardcodes the default (shipped decompile, Boron dock draw 21374).
        if (!isBoron(material)) m.colorHeightCutoff = number("color_height_cutoff", 0.539F);
        m.pomParams = {number("heightstr", 1.0F), number("midlevel", 1.0F),
                       number("aspectratio", 1.0F)};
      }
    }
    if (isMultimat(material)) {
      std::array<unsigned int, 3> arrays{};
      for (std::size_t k = 0; k < kMultimatArrayMaps.size(); ++k) {
        const std::string& path = bitmap(material, kMultimatArrayMaps[k]);
        if (path.empty()) continue;
        arrays[k] = arrayTexturesByUri_
                        .at("native:" +
                            textureKey({normalizeTexturePath(path), multimatArrayIsColor(k), true}))
                        .id;
      }
      m.multimatDiffuse = handleArray(arrays[0]);
      m.multimatNormal = handleArray(arrays[1]);
      m.multimatHeight = handleArray(arrays[2]);
      m.useMultimat =
          static_cast<std::int32_t>(isP1Multimat(material) ? Multimat::P1 : Multimat::P2);
      m.useMultimatDiffuse = arrays[0] != 0 ? 1 : 0;
      m.useMultimatNormal = arrays[1] != 0 ? 1 : 0;
      m.useMultimatHeight = arrays[2] != 0 ? 1 : 0;
      // Neither multimat build applies the aspect ratio.
      m.pomParams = {number("height_detail3dstr", 1.0F), number("midlevel", 1.0F), 1.0F};
      std::array<float, 64> layerStr{};
      for (std::size_t k = 0; k < layerStr.size(); ++k)
        layerStr[k] = number("layer" + std::to_string(k + 1) + "_str", 1.0F);
      m.pomLayerOffset = pomLayers_.add(layerStr) * static_cast<std::uint32_t>(layerStr.size());
    }
    if (emissive != 0) {
      std::array<unsigned int, 2> glow{};
      for (std::size_t k = 0; k < kGlowDetailMaps.size(); ++k)
        glow[k] = textureId(bitmap(material, kGlowDetailMaps[k]), glowDetailIsColor(k));
      const bool animglow = isBoronAnimglow(material);
      if (glow[0] != 0 && (!animglow || glow[1] != 0)) {
        m.glowDetail =
            static_cast<std::int32_t>(animglow ? GlowDetail::Animglow : GlowDetail::Multiply);
        m.glowDetailMap = handle2D(glow[0]);
        m.glowDetail2Map = handle2D(glow[1]);
        // shadergl/ogl/p2_complex_boron_animglow.xml tiles glow_detail by 10, the rest by 3.
        const std::array<float, 2> tiling{animglow ? 10.0F : 3.0F, 20.0F};
        const std::array<std::array<float, 4>*, 2> uvs{&m.glowDetailUv, &m.glowDetail2Uv};
        for (std::size_t k = 0; k < uvs.size(); ++k) {
          const std::string prefix = k == 0 ? "glow_detail" : "glow_detail2";
          *uvs[k] = {number(prefix + "_tiling", tiling[k]), number(prefix + "_tiling_v", tiling[k]),
                     number(prefix + "_speed_u", 1.0F), number(prefix + "_speed_v", 1.0F)};
          m.glowDetailMix[k] = number(prefix + "str", 1.0F);
        }
        m.glowDetailMix[2] = number("baseglow_strength", 1.0F);
        m.glowDetailMix[3] = number("layerweight", 1.0F);
      }
    }
    range.materialId = materials_.add(m);
    materialsDirty_ = true;
  }
  cache_.emplace(streamed.key, std::move(model));
}

GpuUnlitMaterial MeshCache::complexUnlitInfo(const MaterialDef& material, bool vertexColors) {
  const MaterialProperties& props = material.properties;
  const auto number = [&](const char* key, float fallback) {
    const auto it = props.floats.find(key);
    return it != props.floats.end() ? it->second : fallback;
  };
  // sRGB-decoded like the planet port's colour props (p1_complex_unlit samples sRGB).
  const auto color = [&](const char* key) {
    std::array<float, 4> c{1.0F, 1.0F, 1.0F, 1.0F};
    const auto it = props.colors.find(key);
    if (it == props.colors.end()) return c;
    for (std::size_t k = 0; k < c.size(); ++k) c[k] = it->second[k] / 255.0F;
    for (std::size_t k = 0; k < 3; ++k) c[k] = std::pow(c[k], 2.2F);
    return c;
  };
  GpuUnlitMaterial cu;
  const std::uint64_t white = handle2D(0);
  cu.layerMaps = {white, white, white, white};
  cu.diffuseColor = color("diffuse_color");
  cu.colorEmissive = color("color_emissive");
  cu.grade = {number("base_brightness_shift", 1.0F), number("base_contrast_shift", 1.0F),
              number("base_saturation_shift", 1.0F), number("base_hue_shift", 0.0F)};
  cu.cameraFade = {number("camera_fade_range_near_start", 100.0F),
                   number("camera_fade_range_near_end", 1000.0F),
                   number("camera_fade_range_far_start", 100000.0F),
                   number("camera_fade_range_far_end", 120000.0F)};
  cu.angleFade = {number("angle_fade_speed", 0.0F), number("angle_fade_offset", 0.0F)};
  cu.layerStr = {number("diffuse2str", 1.0F), number("diffuse3str", 1.0F),
                 number("alpha1str", 1.0F), number("alpha2str", 1.0F)};
  cu.dblend = {number("dblend1", 1.0F), number("dblend2", 1.0F)};
  cu.userAlphaScale = number("useralphascale", 1.0F);
  cu.vertexColors = vertexColors ? 1 : 0;
  cu.isForcefield = isForcefield(material) ? 1 : 0;
  // complex_unlit_forcefield.frag has its angle fade commented out.
  if (cu.isForcefield != 0) cu.angleFade = {0.0F, 0.0F};
  cu.softRange = {number("softparticle_range_min", 0.0F), number("softparticle_range_max", 0.1F)};
  if (isLightcone(material)) {
    // Defaults from the shaders' ogl xml; no layers or grade.
    cu.lightcone = material.shader == "xu_lightcone_env"     ? 2
                   : material.shader == "xu_lightcone_alpha" ? 3
                                                             : 1;
    const bool env = cu.lightcone == 2;
    cu.cameraFade = {number("camera_fade_range", 100.0F), number("camera_fade_speed", 3.0F),
                     number(env ? "far_fade_range_start" : "camera_fade_range_start", 1000.0F),
                     number(env ? "far_fade_range_stop" : "camera_fade_range_stop", 10000.0F)};
    cu.angleFade = {number("angle_fade_speed", 3.0F), number("angle_fade_offset", 0.0F)};
    cu.softRange = {number("softparticle_range_min", 0.05F),
                    number("softparticle_range_max", 12.0F)};
    cu.grade = {1.0F, 1.0F, 1.0F, 0.0F};
    return cu;
  }
  if (isSimpleUnlit(material)) {
    if (material.shader == "p1_simple_unlit") {
      // simple_unlit.frag: diffuse_color scales the texture rather than standing
      // in for it (both only multiply rgb, so it folds into color_emissive).
      for (std::size_t k = 0; k < cu.colorEmissive.size(); ++k)
        cu.colorEmissive[k] *= cu.diffuseColor[k];
      cu.diffuseColor = {1.0F, 1.0F, 1.0F, 1.0F};
    } else {
      cu.colorEmissive = {1.0F, 1.0F, 1.0F, 1.0F};  // simple_hdr_out.frag has none
    }
    // Neither has layers, a grade, or a camera or angle fade.
    cu.grade = {1.0F, 1.0F, 1.0F, 0.0F};
    cu.cameraFade = {-1.0F, 0.0F, 1.0e30F, 2.0e30F};  // smoothsteps that stay at 1
    cu.angleFade = {0.0F, 0.0F};
    return cu;
  }
  for (std::size_t k = 0; k < kComplexUnlitLayers.size(); ++k) {
    const auto map = props.bitmaps.find(kComplexUnlitLayers[k]);
    if (map == props.bitmaps.end() || map->second.empty()) continue;
    const auto tex = texturesByUri_.find(
        "native:" + textureKey({normalizeTexturePath(map->second), complexUnlitLayerIsColor(k)}));
    if (tex == texturesByUri_.end() || tex->second.id == 0) continue;
    cu.layerMaps[k] = textureHandle(tex->second.id);
    cu.layerMask |= 1 << k;
  }
  return cu;
}

// The driver returns no handle for an incomplete texture: that one samples the stand-in too.
std::uint64_t MeshCache::handle2D(unsigned int id) {
  const std::uint64_t handle = textureHandle(id);
  return handle != 0 ? handle : textureHandle(rlGetTextureIdDefault());
}

std::uint64_t MeshCache::handleArray(unsigned int id) {
  const std::uint64_t handle = textureHandle(id);
  return handle != 0 ? handle : textureHandle(fallbackArray_);
}

std::uint64_t MeshCache::textureHandle(unsigned int id) {
  if (id == 0 || gl_.getTextureHandle == nullptr) return 0;
  const auto [it, inserted] = textureHandles_.try_emplace(id, 0);
  if (inserted) {
    // Freezes the texture's sampler state, final once upload() returned. Resident
    // for the cache's life: textures are never freed before the destructor.
    it->second = gl_.getTextureHandle(id);
    if (it->second != 0) gl_.makeTextureHandleResident(it->second);
  }
  return it->second;
}

const MeshCache::Part* MeshCache::tryLoad(const std::string& meshKey, bool budgeted,
                                          bool optional) {
  const auto it = cache_.find(meshKey);
  if (it != cache_.end()) return it->second.has_value() ? &it->second.value() : nullptr;
  request(meshKey, optional);
  if (budgeted) return nullptr;
  while (cache_.count(meshKey) == 0) pumpStreaming(0.05, std::chrono::milliseconds{5});
  const auto& loaded = cache_.at(meshKey);
  return loaded ? &loaded.value() : nullptr;
}

int MeshCache::uniformLoc(const ::Shader& shader, std::string_view name) const {
  LocTable& table = uniformLocs_[shader.id];
  if (const auto it = table.find(name); it != table.end()) return it->second;
  const std::string key{name};
  const int loc = GetShaderLocation(shader, key.c_str());
  table.emplace(key, loc);
  return loc;
}

const MeshCache::PartFiles& MeshCache::partFiles(const MeshRef& ref) {
  PartEntry& entry = partFiles_[&ref];
  if (entry.settled) return entry.files;
  std::array<const std::string*, 4> have{};
  bool settled = true;
  for (std::size_t level = 0; level < have.size(); ++level) {
    const std::string key = meshKeyLod(ref.meshKey, static_cast<int>(level));
    if (get(key, /*optional=*/level > 0) != nullptr) have[level] = &cache_.find(key)->first;
    settled = settled && isResolved(key);
  }
  for (std::size_t level = 0; level < have.size(); ++level) {
    const std::string* pick = nullptr;
    for (std::size_t l = level; l > 0 && pick == nullptr; --l) pick = have[l];
    entry.files.lod[level] = pick != nullptr ? pick : have[0];
  }
  // X4 drops the fx material (lit windows, signs) from some coarse levels, e.g. the
  // Argon hull part_main lod3 keeps only the hull texture: a level with fewer glowing
  // materials than lod0 is never drawn, the part stays at the finer one.
  if (entry.files.lod[0] != nullptr) {
    const int full = glowingMaterials(*entry.files.lod[0]);
    for (std::size_t level = 1; level < have.size(); ++level)
      if (glowingMaterials(*entry.files.lod[level]) < full)
        entry.files.lod[level] = entry.files.lod[level - 1];
  }
  entry.files.animated = isAnimated(ref.meshKey);
  entry.settled = settled;
  return entry.files;
}

int MeshCache::glowingMaterials(const std::string& meshKey) const {
  const auto model = cache_.find(meshKey);
  if (model == cache_.end() || !model->second) return 0;
  const auto& ranges = model->second->ranges;
  return static_cast<int>(
      std::count_if(ranges.begin(), ranges.end(), [](const Part::Range& r) { return r.glowing; }));
}

const MeshCache::Part* MeshCache::get(const std::string& meshKey, bool optional) {
  return tryLoad(meshKey, /*budgeted=*/true, optional);
}

const MeshCache::Part* MeshCache::warm(const std::string& meshKey, bool optional) {
  return tryLoad(meshKey, /*budgeted=*/false, optional);
}

void MeshCache::bindShadowMap() const {
  for (std::size_t i = 0; i < shadowTex_.size(); ++i) {
    if (shadowTex_[i] == 0) continue;
    rlActiveTextureSlot(kShadowUnits[i]);
    rlEnableTexture(shadowTex_[i]);
  }
  rlActiveTextureSlot(0);
}

void MeshCache::setShadowCascade(int index, unsigned int depthTexId, const ::Matrix& lightVP,
                                 float texelUv) {
  const auto i = static_cast<std::size_t>(index);
  shadowTex_[i] = depthTexId;
  rlActiveTextureSlot(kShadowUnits[i]);
  rlEnableTexture(depthTexId);
  rlActiveTextureSlot(0);
  frame_.lightVP[i] = toMat4(lightVP);
  frame_.shadowTexel[i] = texelUv;
  frameDirty_ = true;
}

void MeshCache::setShadowCascades(int mask) {
  frame_.shadowCascades = mask;
  frameDirty_ = true;
}

void MeshCache::setLightMask(int mask) {
  frame_.lightMask = mask;
  frameDirty_ = true;
}

void MeshCache::setLighting(::Vector3 sunDir, ::Color sunColor, float sunIntensity,
                            ::Color ambientColor, float ambientIntensity, ::Vector3 camPos) {
  // X4's V_lightcolor1 = pow(rgb/255 * intensity, 2.2) (captured: omni
  // 255,255,223 x 1.6 -> 2.81, 2.81, 2.09): the light is linearised as a whole,
  // so the intensity is gamma-decoded along with the colour.
  const auto linear = [](unsigned char c, float scale) {
    return std::pow(static_cast<float>(c) / 255.0F * scale, 2.2F);
  };
  frame_.sunDirection = {sunDir.x, sunDir.y, sunDir.z};
  frame_.sunColor = {linear(sunColor.r, 1.0F), linear(sunColor.g, 1.0F), linear(sunColor.b, 1.0F)};
  frame_.sunIntensity = std::pow(sunIntensity, 2.2F);
  frame_.ambientLight = {linear(ambientColor.r, ambientIntensity),
                         linear(ambientColor.g, ambientIntensity),
                         linear(ambientColor.b, ambientIntensity)};
  frame_.cameraPos = {camPos.x, camPos.y, camPos.z};
  frameDirty_ = true;
}

void MeshCache::captureSceneDepth() {
  prepassDepthValid_ = false;
  visibilityResolved_ = false;
  hasSceneDepth_ = sceneDepth_.capture();
  frame_.hasSceneDepth = hasSceneDepth_ ? 1 : 0;
  frameDirty_ = true;
  if (!hasSceneDepth_) return;
  frame_.depthRange = {static_cast<float>(rlGetCullDistanceNear()),
                       static_cast<float>(rlGetCullDistanceFar())};
  frame_.depthViewport = sceneDepth_.size();
  // Unit 15: past the AO unit and the light buffers (12-14).
  rlActiveTextureSlot(kSceneDepthUnit);
  rlEnableTexture(sceneDepth_.texture());
  rlActiveTextureSlot(0);
}

void MeshCache::setDepthPrepass(bool on) {
  rlDrawRenderBatchActive();
  if (depthPrepass_ && !on) {
    prepassDepthValid_ = true;
    if (visibilityPass_) visibility_->end();
  }
  if (on)
    visibilityPass_ =
        shadersOk_ && visibility_ != nullptr &&
        materials_.items().size() <= VisibilityBuffer::kMaxMaterials &&
        std::max(arena_.vertexBytes(), arena_.indexBytes()) <= visibility_->maxStorageBytes() &&
        visibility_->begin();
  depthPrepass_ = on;
  // The visibility pass's colour is its id target; the plain prepass writes no colour.
  const bool color = !on || visibilityPass_;
  rlColorMask(color, color, color, color);
}

void MeshCache::resolveVisibility() {
  if (!visibilityPass_) return;
  visibilityPass_ = false;
  visibilityResolved_ = true;
  beginInstancedDraws();  // computeSsao changed the frame data
  // The prepass's draw records: nothing has replaced them since.
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, drawDataSsbo_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, uvAnimSsbo_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, VisibilityBuffer::kCommandBinding, indirectBuffer_);
  arena_.bindStorage(VisibilityBuffer::kVertexBinding, VisibilityBuffer::kIndexBinding);
  if (occlusionStatsOn_ && visibleCount_ != 0) {
    if (marksSsbo_ == 0) glGenBuffers(1, &marksSsbo_);
    const std::size_t regions = cullBatch_ ? OcclusionCull::kRegions : 1;
    std::vector<std::uint32_t> marks(visibleCount_ * regions, 0U);
    uploadBuffer(GL_SHADER_STORAGE_BUFFER, marksSsbo_, marks.data(),
                 marks.size() * sizeof(std::uint32_t), GL_STREAM_READ);
    visibility_->markOwners(marksSsbo_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, marksSsbo_);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       static_cast<GLsizeiptr>(marks.size() * sizeof(std::uint32_t)), marks.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    // The commands as drawn: the cull patched their instance counts.
    std::vector<DrawCommand> drawn(commands_.size());
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuffer_);
    glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
                       static_cast<GLsizeiptr>(drawn.size() * sizeof(DrawCommand)), drawn.data());
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    occlusionStats_ = {};
    // Two-phase: the CPU counts of either half are the pre-cull lists.
    const std::size_t preCull = cullDone_ ? commands_.size() / 2 : commands_.size();
    for (std::size_t i = 0; i < preCull; ++i)
      occlusionStats_.preCullIndices +=
          std::uint64_t{commands_[i].count} * commands_[i].instanceCount;
    std::vector<bool> counted(marks.size(), false);
    for (const DrawCommand& c : drawn) {
      std::uint64_t vis = 0;
      for (std::uint32_t i = 0; i < c.instanceCount; ++i) {
        const std::size_t e = c.baseInstance + i;
        vis += marks[e];
        if (!counted[e]) {
          counted[e] = true;
          ++occlusionStats_.entries;
          occlusionStats_.visibleEntries += marks[e];
        }
      }
      occlusionStats_.indices += std::uint64_t{c.count} * c.instanceCount;
      occlusionStats_.visibleIndices += std::uint64_t{c.count} * vis;
    }
  }
  visibility_->resolve(passMvp());
}

void MeshCache::computeSsao() {
  const bool ok = ssaoEnabled_ && ssao_.compute();
  // kFlatFsBody aoMap.
  constexpr int kAoUnit = 11;
  if (ok) {
    rlActiveTextureSlot(kAoUnit);
    rlEnableTexture(ssao_.texture());
    rlActiveTextureSlot(0);
  }
  frame_.aoScreen = ssao_.screenSize();
  setAoEnabled(ok);
}

void MeshCache::setAoEnabled(bool enabled) {
  frame_.useAo = enabled ? 1 : 0;
  frameDirty_ = true;
}

void MeshCache::setPomEnabled(bool enabled) {
  frame_.usePom = enabled ? 1 : 0;
  frameDirty_ = true;
}

void MeshCache::setNormalBinormalSign(int sign) {
  frame_.normalBinormalSign = sign < 0 ? -1 : 1;
  frameDirty_ = true;
}

void MeshCache::commitLocalLights() {
  localLights_.commit();
  localLights_.apply(frame_);
  frameDirty_ = true;
}

void MeshCache::setEnvMap(unsigned int cubemapId, float maxLod) {
  // Persistent binding on unit 9 (kFlatFsBody envMap).
  constexpr int kEnvUnit = 9;
  rlActiveTextureSlot(kEnvUnit);
  rlEnableTextureCubemap(cubemapId);
  rlActiveTextureSlot(0);
  frame_.envMaxLod = maxLod;
  frameDirty_ = true;
}

void MeshCache::packInstances(const std::vector<::Matrix>& xforms) {
  instanceScratch_.clear();
  for (const ::Matrix& m : xforms) instanceScratch_.push_back(toMat4(m));
}

void MeshCache::uploadStationInstances(std::uint64_t revision,
                                       const std::vector<::Matrix>& xforms) {
  packInstances(xforms);
  stationInstances_ = xforms.size();
  const std::size_t total = stationInstances_ + 2 * transientSlotSize_;
  if (instanceSsbo_ == 0) glGenBuffers(1, &instanceSsbo_);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, instanceSsbo_);
  // Respecified, so draws of the old station still in flight keep their storage.
  glBufferData(
      GL_SHADER_STORAGE_BUFFER,
      static_cast<GLsizeiptr>(std::max<std::size_t>(total, 1) * sizeof(instanceScratch_[0])),
      nullptr, GL_DYNAMIC_DRAW);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                  static_cast<GLsizeiptr>(instanceScratch_.size() * sizeof(instanceScratch_[0])),
                  instanceScratch_.data());
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  instancesRevision_ = revision;
  cullHistoryStale_ = true;
}

std::optional<std::uint32_t> MeshCache::uploadTransientInstances(
    TransientSlot slot, const std::vector<::Matrix>& xforms) {
  const std::size_t first = stationInstances_ + static_cast<std::size_t>(slot) * transientSlotSize_;
  if (instanceSsbo_ == 0 || xforms.size() > transientSlotSize_) return std::nullopt;
  packInstances(xforms);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, instanceSsbo_);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                  static_cast<GLintptr>(first * sizeof(instanceScratch_[0])),
                  static_cast<GLsizeiptr>(instanceScratch_.size() * sizeof(instanceScratch_[0])),
                  instanceScratch_.data());
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return static_cast<std::uint32_t>(first);
}

void MeshCache::uploadVisible(const std::vector<std::uint32_t>& instances,
                              const std::vector<VisibleBucket>& cullBuckets) {
  visibleCount_ = instances.size();
  cullBatch_ = occlusion_ != nullptr && !cullBuckets.empty() &&
               cullBuckets.size() <= occlusion_->maxBuckets();
  cullDone_ = false;
  const std::size_t regions = cullBatch_ ? OcclusionCull::kRegions : 1;
  if (visibleSsbo_ == 0) glGenBuffers(1, &visibleSsbo_);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, visibleSsbo_);
  // Respecified, so last frame's draws keep their lists; the cull fills the other regions.
  glBufferData(GL_SHADER_STORAGE_BUFFER,
               static_cast<GLsizeiptr>(
                   std::max<std::size_t>(instances.size() * regions * sizeof(std::uint32_t), 16)),
               nullptr, GL_STREAM_DRAW);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                  static_cast<GLsizeiptr>(instances.size() * sizeof(std::uint32_t)),
                  instances.data());
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  if (!cullBatch_) return;
  std::vector<OcclusionCull::Bucket> buckets;
  buckets.reserve(cullBuckets.size());
  for (const VisibleBucket& b : cullBuckets) {
    OcclusionCull::Bucket& record = buckets.emplace_back();
    record.first = static_cast<std::uint32_t>(b.first);
    record.count = b.count;
    // A part still streaming draws nothing this frame: its box doesn't matter.
    if (const Part* part = get(*b.meshKey)) {
      record.boundsMin = part->boundsMin;
      record.boundsMax = part->boundsMax;
    }
  }
  occlusion_->setBuckets(buckets, stationInstances_ + 2 * transientSlotSize_, cullHistoryStale_);
  cullHistoryStale_ = false;
}

void MeshCache::beginInstancedDraws() {
  rlDrawRenderBatchActive();
  if (frameDirty_) {
    uploadBuffer(GL_UNIFORM_BUFFER, frameUbo_, &frame_, sizeof(frame_));
    frameDirty_ = false;
  }
  if (materialsDirty_) {
    const auto upload = [](unsigned int& buffer, const auto& items) {
      uploadBuffer(GL_SHADER_STORAGE_BUFFER, buffer, items.data(), items.size() * sizeof(items[0]));
    };
    upload(materialSsbo_, materials_.items());
    upload(pomLayerSsbo_, pomLayers_.items());
    upload(unlitMaterialSsbo_, unlitMaterials_.items());
    materialsDirty_ = false;
  }
  glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameUbo_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, instanceSsbo_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, visibleSsbo_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, materialSsbo_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pomLayerSsbo_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, unlitMaterialSsbo_);
  const ::Matrix mvp = passMvp();
  for (const ::Shader* sh : {&instancedShader_, &complexUnlitShader_, &depthInstancedShader_,
                             &depthPrepassShader_, &visibilityShader_, &visibilityMaskShader_})
    setUniformMatrix(*sh, sh->locs[SHADER_LOC_MATRIX_MVP], mvp);
}

// Constant across the pass: only the camera (and the display flip) changes between passes.
::Matrix MeshCache::passMvp() {
  return MatrixMultiply(MatrixMultiply(rlGetMatrixTransform(), rlGetMatrixModelview()),
                        rlGetMatrixProjection());
}

void MeshCache::setUniform(const ::Shader& shader, int loc, const void* value, int type) const {
  if (loc < 0) return;
  const std::size_t size = type == SHADER_UNIFORM_VEC4 ? 16 : 4;
  std::vector<UniformValue>& values = uniformValues_[shader.id];
  if (static_cast<std::size_t>(loc) >= values.size())
    values.resize(static_cast<std::size_t>(loc) + 1);
  UniformValue& v = values[static_cast<std::size_t>(loc)];
  if (v.size == size && std::memcmp(v.bytes.data(), value, size) == 0) return;
  std::memcpy(v.bytes.data(), value, size);
  v.size = size;
  rlEnableShader(shader.id);
  rlSetUniform(loc, value, type, 1);
}

void MeshCache::setUniformMatrix(const ::Shader& shader, int loc, const ::Matrix& m) const {
  if (loc < 0) return;
  std::vector<UniformValue>& values = uniformValues_[shader.id];
  if (static_cast<std::size_t>(loc) >= values.size())
    values.resize(static_cast<std::size_t>(loc) + 1);
  UniformValue& v = values[static_cast<std::size_t>(loc)];
  static_assert(sizeof(::Matrix) <= sizeof(v.bytes));
  if (v.size == sizeof(::Matrix) && std::memcmp(v.bytes.data(), &m, sizeof(::Matrix)) == 0) return;
  std::memcpy(v.bytes.data(), &m, sizeof(::Matrix));
  v.size = sizeof(::Matrix);
  rlEnableShader(shader.id);
  rlSetUniformMatrix(loc, m);
}

std::optional<MeshCache::DrawClass> MeshCache::drawClassOf(const Part::Range& range,
                                                           DrawPass pass) const {
  const bool transparent =
      range.blendClass == BlendClass::Additive || range.blendClass == BlendClass::AlphaBlend;
  if (range.blendClass == BlendClass::Skip || (pass == DrawPass::Transparent) != transparent)
    return std::nullopt;
  // Depth only: additive/blend fx never occlude (the opaque pass never holds them);
  // masked cutouts cast as solid (no alpha test in the depth stage, an accepted v1
  // approximation).
  if (shadowPass_) return DrawClass::Depth;
  const bool prepassRange = range.blendClass == BlendClass::Opaque && !range.unlit;
  // The visibility pass also takes the cutouts (alpha-tested ids). The mask classes draw
  // unculled, so TWOSIDED ranges ride along.
  const bool cutoutRange = (range.blendClass == BlendClass::Mask || range.twoSided) && !range.unlit;
  if (depthPrepass_) {
    // Before the Prepass test: a TWOSIDED range is also Opaque, but must draw unculled.
    if (cutoutRange && visibilityPass_) return DrawClass::PrepassMask;
    if (prepassRange) return DrawClass::Prepass;
    return std::nullopt;
  }
  // The visibility buffer already shaded exactly the visibility pass's ranges.
  if (visibilityResolved_ && (prepassRange || cutoutRange)) return std::nullopt;
  if (range.unlit) {
    if (!transparent) return DrawClass::UnlitOpaque;
    if (range.blendClass == BlendClass::AlphaBlend) return DrawClass::UnlitAlphaBlend;
    return range.screen ? DrawClass::UnlitScreen : DrawClass::UnlitBlended;
  }
  switch (range.blendClass) {
    case BlendClass::Mask:
      return DrawClass::LitMask;
    case BlendClass::AlphaBlend:
      return DrawClass::LitAlphaBlend;
    case BlendClass::Additive:
      return DrawClass::LitAdditive;
    default:
      return range.twoSided ? DrawClass::LitMask : DrawClass::LitOpaque;
  }
}

void MeshCache::submitClass(DrawClass cls, std::size_t base, std::size_t count) {
  const ::Shader* shader = &instancedShader_;
  switch (cls) {
    case DrawClass::Depth:
      shader = &depthInstancedShader_;
      break;
    case DrawClass::Prepass:
      shader = visibilityPass_ ? &visibilityShader_ : &depthPrepassShader_;
      break;
    case DrawClass::PrepassMask:
      shader = &visibilityMaskShader_;
      break;
    case DrawClass::UnlitOpaque:
    case DrawClass::UnlitBlended:
    case DrawClass::UnlitScreen:
    case DrawClass::UnlitAlphaBlend:
      shader = &complexUnlitShader_;
      break;
    default:
      break;
  }
  const auto drawBase = static_cast<int>(base);
  setUniform(*shader, uniformLoc(*shader, "drawBase"), &drawBase, SHADER_UNIFORM_INT);

  const bool blended = cls == DrawClass::LitAlphaBlend || cls == DrawClass::LitAdditive ||
                       cls == DrawClass::UnlitBlended || cls == DrawClass::UnlitScreen ||
                       cls == DrawClass::UnlitAlphaBlend;
  if (cls == DrawClass::UnlitBlended) {
    // The shader premultiplies its fades into rgb: X4's ADDITIVE is One/One.
    rlSetBlendFactors(RL_ONE, RL_ONE, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM);
  } else if (cls == DrawClass::UnlitScreen) {
    rlSetBlendFactors(RL_ONE, RL_ONE_MINUS_SRC_COLOR, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM);
  } else if (cls == DrawClass::UnlitAlphaBlend) {
    // The lightcone_alpha shader carries its fades in alpha, not rgb.
    rlSetBlendFactors(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM);
  } else if (cls == DrawClass::LitAdditive) {
    BeginBlendMode(BLEND_ADDITIVE);
  } else if (cls == DrawClass::LitAlphaBlend) {
    BeginBlendMode(BLEND_ALPHA);
  }
  // The lit shader can discard and writes gl_SampleMask, which with depth writes on
  // forces late depth testing: every hidden fragment would be shaded despite the
  // prepass. Its depth is already there, so test only.
  const bool testOnly = cls == DrawClass::LitOpaque && prepassDepthValid_;
  if (blended || testOnly) rlDisableDepthMask();
  // X4 shows the backs of its one-sided cutout panels (solar panels) and TWOSIDED hulls.
  const bool unculled = cls == DrawClass::PrepassMask || cls == DrawClass::LitMask;
  const GLboolean culling = glIsEnabled(GL_CULL_FACE);
  if (unculled) glDisable(GL_CULL_FACE);
  rlEnableShader(shader->id);
  // After the blend switch: BeginBlendMode flushes raylib's batch, which unbinds the VAO.
  arena_.bind();
  // GL takes the indirect buffer offset as a pointer.
  const auto* offset = reinterpret_cast<const void*>(  // NOLINT(performance-no-int-to-ptr)
      static_cast<std::uintptr_t>(base) * sizeof(DrawCommand));
  glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, offset, static_cast<GLsizei>(count),
                              0);
  if (unculled && culling == GL_TRUE) glEnable(GL_CULL_FACE);
  if (blended || testOnly) rlEnableDepthMask();
  if (blended) EndBlendMode();
}

void MeshCache::drawInstanced(const std::vector<InstancedDraw>& draws, ::Color tint,
                              DrawPass pass) {
  if (!shadersOk_ || instanceSsbo_ == 0 || visibleSsbo_ == 0) return;
  std::array<std::vector<std::uint32_t>, kDrawClasses> classBuckets;
  for (std::size_t c = 0; c < kDrawClasses; ++c) {
    classCommands_[c].clear();
    classDrawData_[c].clear();
  }
  uvSlots_.assign(1, UvTransforms{});
  // The main view's batch: its prepass culls in two phases, later passes draw the survivors.
  const bool cull = cullBatch_ && !shadowPass_;
  const bool twoPhase = cull && depthPrepass_;
  const bool culled = cull && !depthPrepass_ && cullDone_;
  // The visibility pass's records are the resolve's too: those need the uv slots.
  const bool depthOnly = shadowPass_ || (depthPrepass_ && !visibilityPass_);
  const std::uint32_t packedTint =
      static_cast<std::uint32_t>(tint.r) | static_cast<std::uint32_t>(tint.g) << 8U |
      static_cast<std::uint32_t>(tint.b) << 16U | static_cast<std::uint32_t>(tint.a) << 24U;
  for (const InstancedDraw& draw : draws) {
    if (draw.count == 0) continue;
    const Part* part = get(*draw.meshKey);
    // Most parts draw in only one of the two passes: skip them before any setup.
    if (part == nullptr || !part->inPass[static_cast<std::size_t>(pass)]) continue;
    DrawData data;
    data.lodFade = draw.fade;
    data.tint = packedTint;
    data.flags = (draw.invert ? kDrawFadeInvert : 0U) | (draw.still ? kDrawStill : 0U) |
                 (part->hasNormals ? kDrawHasNormals : 0U);
    if (!depthOnly) {
      if (const auto it = uvAnimations_.find(*draw.meshKey); it != uvAnimations_.end()) {
        data.uvSlot = static_cast<std::uint32_t>(uvSlots_.size());
        uvSlots_.push_back(evalUvAnimation(it->second, draw.still ? 0.0 : animTime_));
      }
    }
    for (const Part::Range& range : part->ranges) {
      const std::optional<DrawClass> cls = drawClassOf(range, pass);
      if (!cls) continue;
      const auto c = static_cast<std::size_t>(*cls);
      classCommands_[c].push_back({range.indexCount, draw.count, range.firstIndex, range.baseVertex,
                                   static_cast<std::uint32_t>(draw.first)});
      data.materialId = range.materialId;
      classDrawData_[c].push_back(data);
      classBuckets[c].push_back(draw.bucket);
    }
  }
  commands_.clear();
  drawData_.clear();
  std::vector<std::uint32_t> countIndex;
  for (std::size_t c = 0; c < kDrawClasses; ++c) {
    commands_.insert(commands_.end(), classCommands_[c].begin(), classCommands_[c].end());
    drawData_.insert(drawData_.end(), classDrawData_[c].begin(), classDrawData_[c].end());
    if (twoPhase || culled)
      for (const std::uint32_t bucket : classBuckets[c])
        countIndex.push_back(occlusion_->countIndex(
            twoPhase ? OcclusionCull::kPhaseA : OcclusionCull::kVisible, bucket));
  }
  if (commands_.empty()) return;
  // Each sub-draw reads its bucket's slice of the cull's region; the second phase is a
  // copy of every command with its own draw index, so the ids keep the two apart.
  const std::size_t half = commands_.size();
  const auto entries = static_cast<std::uint32_t>(visibleCount_);
  if (twoPhase || culled) {
    const std::uint32_t region = twoPhase ? OcclusionCull::kPhaseA : OcclusionCull::kVisible;
    for (DrawCommand& cmd : commands_) cmd.baseInstance += region * entries;
  }
  std::vector<std::uint32_t> countIndexB;
  if (twoPhase) {
    commands_.reserve(2 * half);
    drawData_.reserve(2 * half);
    for (std::size_t i = 0; i < half; ++i) {
      DrawCommand cmd = commands_[i];
      cmd.baseInstance += (OcclusionCull::kPhaseB - OcclusionCull::kPhaseA) * entries;
      commands_.push_back(cmd);
      drawData_.push_back(drawData_[i]);
    }
    for (std::size_t c = 0; c < kDrawClasses; ++c)
      for (const std::uint32_t bucket : classBuckets[c])
        countIndexB.push_back(occlusion_->countIndex(OcclusionCull::kPhaseB, bucket));
  }
  // The ids hold 16 bits of draw index and of instance; past that, a plain prepass.
  if (depthPrepass_ && visibilityPass_ &&
      (commands_.size() >= 0xFFFFU ||
       std::any_of(commands_.begin(), commands_.end(),
                   [](const DrawCommand& c) { return c.instanceCount > 0x10000U; }))) {
    visibility_->end();
    visibilityPass_ = false;
    rlColorMask(false, false, false, false);
  }

  beginInstancedDraws();
  uploadBuffer(GL_SHADER_STORAGE_BUFFER, drawDataSsbo_, drawData_.data(),
               drawData_.size() * sizeof(DrawData), GL_STREAM_DRAW);
  uploadBuffer(GL_SHADER_STORAGE_BUFFER, uvAnimSsbo_, uvSlots_.data(),
               uvSlots_.size() * sizeof(UvTransforms), GL_STREAM_DRAW);
  uploadBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuffer_, commands_.data(),
               commands_.size() * sizeof(DrawCommand), GL_STREAM_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, drawDataSsbo_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, uvAnimSsbo_);
  glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuffer_);
  const auto submitAll = [&](std::size_t base) {
    for (std::size_t c = 0; c < kDrawClasses; ++c) {
      const std::size_t count = classCommands_[c].size();
      // A fallen-back visibility pass leaves the cutouts to the lit pass (no resolve).
      const bool skip = static_cast<DrawClass>(c) == DrawClass::PrepassMask && !visibilityPass_;
      if (count != 0 && !skip) submitClass(static_cast<DrawClass>(c), base, count);
      base += count;
    }
  };
  if (twoPhase) {
    const ::Matrix mvp = passMvp();
    occlusion_->cullPhaseA(mvp, visibleSsbo_, entries);
    occlusion_->patchCommands(indirectBuffer_, 0, countIndex);
    submitAll(0);
    if (cullStepHook_) cullStepHook_(CullStep::PhaseA);
    const bool hiz = occlusion_->buildHiz();
    if (cullStepHook_) cullStepHook_(CullStep::Hiz);
    occlusion_->cullPhaseB(mvp, visibleSsbo_, entries, hiz);
    occlusion_->patchCommands(indirectBuffer_, half, countIndexB);
    if (cullStepHook_) cullStepHook_(CullStep::Cull);
    submitAll(half);
    cullDone_ = true;
  } else {
    if (culled) occlusion_->patchCommands(indirectBuffer_, 0, countIndex);
    submitAll(0);
  }
  rlDisableVertexArray();
  glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

}  // namespace x4sb::editor

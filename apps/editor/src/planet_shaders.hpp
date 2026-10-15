#pragma once
// Planet, atmosphere, ring and aurora programs, plus the colour helpers sky.cpp and
// mesh_cache.cpp splice in. Uniform names are X4 material property names (S_/U_ prefix)
// because that is how planets.cpp binds game data; keep them.

namespace x4sb::editor::planet_glsl {

// Ground-from-space scattering per vertex (O'Neil, GPU Gems 2 ch. 16, two samples).
constexpr const char* kBodyVs = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec4 vertexTangent;
in vec2 vertexTexCoord;
in vec2 vertexTexCoord2;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 worldRot;
uniform vec3 worldPos;
uniform vec3 V_direction1;
uniform float F_time;
uniform vec4 uvLinear[4];
uniform vec2 uvOffset[4];
uniform float U_cloud_speed;
uniform float U_animate_uv0;
uniform float U_wavelength_red;
uniform float U_wavelength_green;
uniform float U_wavelength_blue;
uniform float U_fouterradius;
uniform float U_finnerradius;
uniform float U_m_kr;
uniform float U_m_km;
uniform float U_m_esun;
uniform float U_fscaledepth;
out vec2 vUv0;
out vec2 vUv1;
out vec3 vInscatter;
out vec3 vExtinction;
out vec3 vToEye;
out vec3 vNormal;
out vec3 vTangent;
out vec3 vBitangent;
out float vTint;

const float kPi = 3.14159265359;

float opticalDepth(float cosAngle, float scaleDepth) {
  float x = 1.0 - cosAngle;
  return scaleDepth * exp(-0.00287 + x * (0.459 + x * (3.83 + x * (-6.80 + x * 5.25))));
}

void main() {
  mat3 rot = mat3(worldRot);
  // Planet-centred: worldPos is ~1e8 m, so never build the absolute vertex position.
  vec3 rel = rot * vertexPosition;
  vec3 eye = -worldPos;
  float outer = U_fouterradius;
  float inner = U_finnerradius;
  float depth = U_fscaledepth;

  vec3 dir = rel - eye;
  float len = length(dir);
  dir /= len;
  float b = 2.0 * dot(eye, dir);
  float c = dot(eye, eye) - outer * outer;
  float entry = 0.5 * (-b - sqrt(max(0.0, b * b - 4.0 * c)));
  vec3 start = eye + dir * entry;
  len -= entry;

  vec3 invWave4 = 1.0 / pow(vec3(U_wavelength_red, U_wavelength_green, U_wavelength_blue) +
                                vec3(1e-5), vec3(4.0));
  float scale = 1.0 / (outer - inner);
  float scaleOverDepth = scale / depth;
  float r = length(rel);
  float camCos = dot(-dir, rel) / r;
  float lightCos = dot(V_direction1, rel) / r;
  float camOffset = exp((inner - outer) / depth) * opticalDepth(camCos, depth);
  float angleSum = opticalDepth(lightCos, depth) + opticalDepth(camCos, depth);
  vec3 extinctK = invWave4 * (U_m_kr * 4.0 * kPi) + U_m_km * 4.0 * kPi;

  float segLen = len * 0.5;
  vec3 p = start + dir * (segLen * 0.5);
  vec3 sum = vec3(0.0);
  vec3 att = vec3(0.0);
  for (int i = 0; i < 2; ++i) {
    float density = exp(scaleOverDepth * (inner - length(p)));
    att = exp(-(density * angleSum - camOffset) * extinctK);
    sum += att * density * segLen * scale;
    p += dir * segLen;
  }
  vInscatter = sum * (invWave4 * U_m_kr * U_m_esun + U_m_km * U_m_esun);
  vExtinction = att;

  vToEye = -(worldPos + rel);
  vNormal = rot * vertexNormal;
  vTangent = rot * vertexTangent.xyz;
  vBitangent = rot * (cross(vertexNormal, vertexTangent.xyz) * vertexTangent.w);
  vUv0 = mat2(uvLinear[0]) * vertexTexCoord + uvOffset[0];
  vUv0.x += F_time * U_cloud_speed * 0.5 * U_animate_uv0;
  vUv1 = mat2(uvLinear[1]) * vertexTexCoord2 + uvOffset[1];
  vTint = vertexColor.r;
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

// Shared by every planet fragment program, the sky's flare sprites and the station's
// complex-unlit/lightcone path; mesh_cache.cpp calls saturate() by that name.
constexpr const char* kSharedFs = R"(
float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }
vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }
vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Overlay whose multiply/screen switch is a 0.1-wide ramp around 0.45 base luminance.
vec3 softOverlay(vec3 base, vec3 blend) {
  float w = saturate(10.0 * (luma(base) - 0.45));
  return mix(2.0 * base * blend, 1.0 - 2.0 * (1.0 - base) * (1.0 - blend), w);
}
float softOverlay(float base, float blend) {
  float w = saturate(10.0 * (base - 0.45));
  return mix(2.0 * base * blend, 1.0 - 2.0 * (1.0 - base) * (1.0 - blend), w);
}

// Brightness, contrast about 0.5, saturation (Haeberli weights), then a hue turn of -hue
// degrees about the grey axis, folded into one affine map: use as gradeMatrix(..) * vec4(rgb, 1).
mat4x3 gradeMatrix(float brightness, float contrast, float saturation, float hue) {
  vec3 w = (1.0 - saturation) * vec3(0.3086, 0.6094, 0.0820);
  mat3 sat = mat3(w.r + saturation, w.r, w.r,
                  w.g, w.g + saturation, w.g,
                  w.b, w.b, w.b + saturation);
  float a = -radians(hue);
  float cs = cos(a);
  float sn = sin(a) * 0.57735027;
  float k = (1.0 - cs) / 3.0;
  mat3 turn = mat3(cs + k, k + sn, k - sn,
                   k - sn, cs + k, k + sn,
                   k + sn, k - sn, cs + k);
  mat3 lin = turn * sat * (brightness * contrast);
  vec3 offset = turn * sat * vec3(0.5 - 0.5 * contrast);
  return mat4x3(lin[0], lin[1], lin[2], offset);
}
)";

constexpr const char* kBodyFsHeader = R"(#version 330
in vec2 vUv0;
in vec2 vUv1;
in vec3 vInscatter;
in vec3 vExtinction;
in vec3 vToEye;
in vec3 vNormal;
in vec3 vTangent;
in vec3 vBitangent;
in float vTint;
out vec4 OUT_RT0;
uniform vec3 V_direction1;
uniform vec3 V_lightcolor1;
uniform vec3 V_ambient1;
uniform float F_globallightscale;
uniform float F_time;
uniform float F_alphascale;
uniform bool B_vertexdata0;
)";

constexpr const char* kBodyFsHelpers = R"(
vec3 screen(vec3 a, vec3 b) { return 1.0 - (1.0 - a) * (1.0 - b); }
vec3 amix(vec3 a, vec3 b, float t) { return mix(a, b, saturate(t)); }
vec3 gammaDecode(vec3 c) { return pow(c + vec3(1e-5), vec3(2.2)); }
vec4 gammaDecode(vec4 c) { return vec4(gammaDecode(c.rgb), c.a); }

// Snow/ice texels (all channels near white) stay white; the rest tint both ways.
vec3 climateBlend(vec3 a, vec3 b) {
  if (all(greaterThan(a, vec3(0.99)))) return vec3(1.0);
  return a * b + (a + b) * 0.25;
}

float rim(vec3 v, vec3 n, float power) { return pow(max(1.0 - abs(dot(v, n)), 0.0), power); }

// `top` over `bottom`, each weighted by its own alpha.
vec4 over(vec4 top, vec4 bottom) {
  return vec4(top.rgb * top.a + bottom.rgb * (1.0 - top.a), min(top.a + bottom.a, 1.0));
}

vec2 normalXY(sampler2D map, vec2 uv, float strength) {
  return (2.0 * texture(map, uv).xy - 1.0) * strength;
}
vec3 normalFull(sampler2D map, vec2 uv, float strength) {
  vec2 xy = normalXY(map, uv, strength);
  return vec3(xy, sqrt(1.0 - saturate(dot(xy, xy))));
}
vec3 toWorld(vec3 t) {
  return normalize(t.x * normalize(vTangent) + t.y * normalize(vBitangent) +
                   t.z * normalize(vNormal));
}

vec3 atmosphereK3() { return 1.0 - exp(-(vInscatter + 0.25 * vExtinction)); }

// Strauss diffuse (transparency 0) plus a Ward-style anisotropic lobe whose frame is
// built from world +X; n is used as given (some bodies pass an unnormalized blend).
vec3 bodyLight(vec3 n, vec3 v, float gloss, float metal, vec3 albedo, vec2 aniso) {
  vec3 l = normalize(V_direction1);
  vec3 h = normalize(l + v);
  float nl = dot(n, l);
  float nv = dot(n, v);
  vec2 rough = aniso + vec2(1e-5);
  vec3 ta = normalize(cross(n, vec3(1.0, 0.0, 0.0)));
  vec3 tb = normalize(cross(n, ta));
  vec2 hs = vec2(dot(h, ta), dot(h, tb)) / rough;
  float lobe = exp(-2.0 * dot(hs, hs) / (1.0 + dot(h, n))) /
               (4.0 * 3.14159 * rough.x * rough.y * sqrt(abs(nl * nv))) * (gloss + 0.001);
  vec3 light = V_lightcolor1 * F_globallightscale;
  vec3 diffuse = max(vec3(0.0), nl * (1.0 - metal * gloss) *
                                    (1.0 - gloss * gloss * gloss) * albedo * light);
  vec3 spec = (lobe * saturate(1.00001 - metal) + lobe * saturate(metal + 0.0001) * albedo) * light;
  spec = max(vec3(0.0), spec * pow(0.5 * nl + 0.5, 6.2));
  return saturate(diffuse + spec) + V_ambient1 * albedo;
}
)";

constexpr const char* kEarthlikeFs = R"(uniform vec4 S_diffuse_color;
uniform bool S_diffuse_bool;
uniform float S_diffusestr;
uniform sampler2D S_diffuse_map;
uniform float U_diffoffsetu;
uniform bool S_diffuse_detail_bool;
uniform sampler2D S_diffuse_detail_map;
uniform float S_diffuse_detailstr;
uniform float S_diffuse_detail_tiling;
uniform float S_diffuse_detail_tiling_v;
uniform float U_diffdetailoffsetu;
uniform float U_diffdetailoffsetv;
uniform sampler2D S_polar_map;
uniform float U_polarscale;
uniform vec4 U_color_water;
uniform float U_waterlevel;
uniform sampler2D S_color_height_map;
uniform sampler2D S_color_climate_map;
uniform bool S_citymap_bool;
uniform sampler2D S_citymap_map;
uniform float S_citymap_tiling;
uniform float S_citymap_tiling_v;
uniform float U_citylevel;
uniform sampler2D S_color_glow_map;
uniform vec4 S_color_glow_color;
uniform float S_color_glowstr;
uniform bool S_color_extraglow_bool;
uniform sampler2D S_color_extraglow_map;
uniform vec4 S_color_extraglow_color;
uniform float S_color_extraglowstr;
uniform float S_color_extraglow_tiling;
uniform float S_color_extraglow_tiling_v;
uniform float U_extraglowlevel;
uniform bool S_normal_bool;
uniform sampler2D S_normal_map;
uniform float S_normalstr;
uniform bool S_normal_detail_bool;
uniform sampler2D S_normal_detail_map;
uniform float S_normal_detailstr;
uniform float S_normal_detail_tiling;
uniform float S_normal_detail_tiling_v;
uniform vec4 U_color_atmo;
uniform float U_cloud_speed;
uniform float U_cloud_colorfactor;
uniform bool S_cloudmap_bool;
uniform sampler2D S_cloudmap_map;
uniform float S_cloudmapstr;
uniform float U_cloudsdiffoffsetu;
uniform bool S_clouds_detail_mask_bool;
uniform sampler2D S_clouds_detail_mask_map;
uniform float S_clouds_detail_maskstr;
uniform float S_clouds_detail_mask_tiling;
uniform float S_clouds_detail_mask_tiling_v;
uniform float U_cloudsdiffdetailoffsetv;
uniform float U_cloudcutoff;
uniform sampler2D S_densecloudmap_map;
uniform float U_densecutoff;
uniform float U_anisox;
uniform float U_anisoy;
uniform float U_base_brightness_shift;
uniform float U_base_contrast_shift;
uniform float U_base_saturation_shift;
uniform float U_base_hue_shift;
uniform float U_fdensity;

#ifdef XENON
const vec4 kCityScales = vec4(20.0, 10.0, 125.0, 350.0);
const float kGlowKey = 8.0;
#else
const vec4 kCityScales = vec4(1000.0, 100.0, 25.0, 35.0);
const float kGlowKey = 6.0;
#endif

// Component-wise max of two drifting cloud copies, a tiled detail copy and three dense layers.
vec4 cloudLayers(vec2 uv, vec4 base, float cloudW, float denseW, float detailW) {
  float drift = 0.75 * U_cloud_speed * F_time;
  float o = U_cloudsdiffoffsetu;
  vec2 tile = vec2(S_clouds_detail_mask_tiling, S_clouds_detail_mask_tiling_v);
  vec4 c = max(base, cloudW * texture(S_cloudmap_map, uv + vec2(o + drift, -0.1 * drift)));
  c = max(c, cloudW * texture(S_cloudmap_map, uv + vec2(o + 0.25 + drift, 0.15 - 0.1 * drift)));
  c = max(c, detailW * cloudW *
                 texture(S_cloudmap_map,
                         vec2(uv.x * tile.x + o + 0.9 * drift * tile.x,
                              uv.y * tile.y + U_cloudsdiffdetailoffsetv + 0.1 * drift * tile.y)));
  vec2 du = uv + vec2(o, 0.0);
  c = max(c, denseW * texture(S_densecloudmap_map, du + vec2(0.5 + drift, 0.125)));
  c = max(c, denseW * texture(S_densecloudmap_map, du + vec2(-0.15 + drift, -0.25)));
  c = max(c, denseW * texture(S_densecloudmap_map, du + vec2(0.25 + drift, 0.0)));
  return c;
}

void main() {
  float t = F_time;
  float speed = U_cloud_speed;
  float ps = U_polarscale;
  float wl = U_waterlevel;
  vec3 nv = normalize(vNormal);
  vec3 sunDir = V_direction1;
  vec2 detailOffset = vec2(U_diffdetailoffsetu, U_diffdetailoffsetv);
  vec2 baseUv = vUv0 + vec2(U_diffoffsetu, 0.0);
  float gloss = 0.1;
  float metal = 0.0;
  vec2 aniso = vec2(U_anisox, U_anisoy);

  vec4 albedo = S_diffuse_color;
  if (S_diffuse_bool) albedo = S_diffusestr * texture(S_diffuse_map, baseUv);
  float detailW = S_diffuse_detailstr;
  float cloudDetailW = S_clouds_detail_maskstr;
  if (B_vertexdata0) {
    detailW *= vTint;
    cloudDetailW *= vTint;
  }
  vec2 detailUv = vUv0 * vec2(S_diffuse_detail_tiling, S_diffuse_detail_tiling_v) + detailOffset;
  if (S_diffuse_detail_bool)
    albedo.rgb = (albedo.rgb + detailW * texture(S_diffuse_detail_map, detailUv).rgb) /
                 (S_diffusestr + detailW);
  albedo = saturate(albedo);

  vec4 polar = texture(S_polar_map, vUv0);
  float ice = smoothstep(ps - 0.01, ps, polar.a + (albedo.r + 0.2) * 0.2 * sign(ps)) *
              -sign(ps - 1.0);
  float ground = 1.0 - ice;

  vec4 glow = S_color_glow_color;
  vec3 n = nv;
  vec3 surface = polar.rgb * ice * 1.5;
  if (albedo.r < wl) {
    vec3 water = U_color_water.rgb;
    surface += max(water, water * (2.01 - wl + 1.52 * albedo.r));
    surface += max(vec3(0.0), (albedo.r - 0.85 * wl) * vec3(1.0, 1.0, 0.5));
    gloss = 0.5 * ground;
  } else {
    surface += albedo.rgb;
    vec3 height = texture(S_color_height_map, vec2((albedo.r - wl) / (1.0 - wl), 0.5)).rgb;
    surface *= height;
    vec2 climateUv = vec2(2.0 * vUv0.x, vUv0.y + sign(vUv0.y - 0.5) * (0.5 - ps * 0.5));
    surface = climateBlend(surface, texture(S_color_climate_map, climateUv).rgb);

    vec2 cityUv = vUv0 * vec2(S_citymap_tiling, S_citymap_tiling_v) + detailOffset;
    if (S_citymap_bool) {
      glow = texture(S_color_glow_map, cityUv);
      glow.rgb *= S_color_glowstr;
      vec3 city = 0.5 * (texture(S_citymap_map, kCityScales.x * cityUv).rgb +
                         texture(S_citymap_map, kCityScales.y * cityUv).rgb);
      float cityA = texture(S_citymap_map, kCityScales.z * cityUv).a *
                    texture(S_citymap_map, kCityScales.w * cityUv).a;
      float populated = glow.a > 1.0 - U_citylevel ? 1.0 : 0.0;
      glow.rgb *= populated;
      if (populated * cityA > 0.04) {
#ifdef XENON
        surface = city;
        metal = 0.8;
        aniso = vec2(0.75);
#else
        surface = softOverlay(surface, city);
#endif
      }
    }
#ifndef XENON
    if (S_color_extraglow_bool) {
      vec4 extra = texture(S_color_extraglow_map,
                           vUv0 * vec2(S_color_extraglow_tiling, S_color_extraglow_tiling_v) +
                               detailOffset);
      extra.rgb *= S_color_extraglowstr;
      extra.rgb *= extra.a > 1.0 - U_extraglowlevel ? 1.0 : 0.0;
      glow.rgb += extra.rgb;
    }
#endif

    if (S_normal_bool) {
      vec3 nt = normalFull(S_normal_map, baseUv, S_normalstr);
      if (S_normal_detail_bool) {
        vec2 nd = normalXY(S_normal_detail_map, detailUv, S_normalstr) +
                  normalXY(S_normal_detail_map,
                           vUv0 * vec2(S_normal_detail_tiling, S_normal_detail_tiling_v) +
                               detailOffset,
                           0.75);
        nt.xy += nd * S_normal_detailstr;
      }
#ifndef XENON
      nt = normalize(nt);
#endif
      n = toWorld(nt);
    }
    n = normalize(0.5 * ground * n + 0.5 * ice * nv);
    // Mountain peaks take the polar colour too.
    ice = saturate(ice + smoothstep(0.8, 0.99, height.b));
    ground = 1.0 - ice;
    gloss = 0.1 * ground;
  }
  surface = surface * ground + polar.rgb * ice * 1.5;

  vec3 v = normalize(vToEye);

  vec4 mask = S_diffuse_color;
  if (S_cloudmap_bool)
    mask = S_cloudmapstr *
           texture(S_cloudmap_map, vUv1 + vec2(U_cloudsdiffoffsetu + t * speed, 0.5 * t * speed));
  if (S_clouds_detail_mask_bool) {
    vec2 tile = vec2(S_clouds_detail_mask_tiling, S_clouds_detail_mask_tiling_v);
    vec4 dm = cloudDetailW * cloudDetailW *
              texture(S_clouds_detail_mask_map,
                      vec2(vUv1.x * tile.x + U_cloudsdiffoffsetu + 1.3 * t * speed * tile.x,
                           vUv1.y * tile.y + U_cloudsdiffdetailoffsetv -
                               0.125 * t * speed * tile.y));
    mask.rgb = (mask.rgb + dm.rgb) / (S_cloudmapstr + cloudDetailW);
  }
  mask = saturate(mask);

  vec4 haze = U_color_atmo;
  haze.a += rim(v, nv, 6.0) * pow(U_color_atmo.a, 0.3);
  haze.a -= (1.0 - rim(v, nv, 0.2)) * U_color_atmo.a * 0.5;

  float cloudW = smoothstep(U_cloudcutoff - 0.4, U_cloudcutoff, mask.r);
  float denseW = smoothstep(U_densecutoff - 0.4, U_densecutoff, mask.r);

  vec4 clouds = cloudLayers(vUv1, haze, cloudW, denseW, cloudDetailW);
  clouds.rgb = saturate(screen(clouds.rgb, haze.rgb));
  clouds.rgb = amix(clouds.rgb, U_color_atmo.rgb, U_cloud_colorfactor);

  float sunN = dot(sunDir, nv);
  vec3 shade = vec3(1.0);
  if (clouds.a > 0.85) shade -= mix(1.0, 0.0, sunN) * 0.65 * clouds.a;
#ifdef XENON
  vec2 shadowUv = vUv1 + 0.001 * normalize(nv - sunDir).xy;
#else
  vec2 shadowUv = vUv1 + 0.001 * normalize(nv + sunDir).xy;
#endif
  vec4 caster =
      cloudLayers(shadowUv, vec4(haze.rgb, U_color_atmo.a), cloudW, denseW, cloudDetailW);
  if (caster.a > 0.25) shade -= mix(1.0, 0.05, sunN) * 0.75 * caster.a;

  aniso *= 1.0 - gloss;
  surface = saturate(gradeMatrix(U_base_brightness_shift, U_base_contrast_shift,
                                 U_base_saturation_shift, U_base_hue_shift) *
                     vec4(surface, 1.0));
  vec3 col = bodyLight(n, v, gloss, metal, surface, aniso);

  vec3 k3 = atmosphereK3();
  col = col * k3.b + col * (1.0 - U_fdensity) + k3 * U_fdensity;
  col += saturate(1.0 - luma(col * kGlowKey)) * glow.rgb * ground;
  col = amix(col * shade, clouds.rgb * 2.0 * k3 + k3, clouds.a);
  OUT_RT0 = vec4(gammaDecode(saturate(col)), S_diffuse_color.a * F_alphascale);
}
)";

constexpr const char* kMoonFs = R"(uniform vec4 S_diffuse_color;
uniform bool S_diffuse_bool;
uniform float S_diffusestr;
uniform sampler2D S_diffuse_map;
uniform float U_diffoffsetu;
uniform bool S_diffuse_detail_bool;
uniform sampler2D S_diffuse_detail_map;
uniform float S_diffuse_detailstr;
uniform float S_diffuse_detail_tiling;
uniform float S_diffuse_detail_tiling_v;
uniform float U_diffdetailoffsetu;
uniform float U_diffdetailoffsetv;
uniform sampler2D S_polar_map;
uniform float U_polarscale;
uniform float U_icecolscale;
uniform vec4 U_color_plains;
uniform float U_waterlevel;
uniform bool S_citymap_bool;
uniform sampler2D S_citymap_map;
uniform float S_citymap_tiling;
uniform float S_citymap_tiling_v;
uniform float U_citylevel;
uniform sampler2D S_color_glow_map;
uniform vec4 S_color_glow_color;
uniform float S_color_glowstr;
uniform bool S_normal_bool;
uniform sampler2D S_normal_map;
uniform float S_normalstr;
uniform bool S_normal_detail_bool;
uniform sampler2D S_normal_detail_map;
uniform float S_normal_detailstr;
uniform float S_normal_detail_tiling;
uniform float S_normal_detail_tiling_v;
uniform vec4 U_color_atmo;
uniform float U_cloud_speed;
uniform bool S_clouds_mask_bool;
uniform sampler2D S_clouds_mask_map;
uniform float S_clouds_maskstr;
uniform float U_cloudsdiffoffsetu;
uniform bool S_clouds_detail_mask_bool;
uniform sampler2D S_clouds_detail_mask_map;
uniform float S_clouds_detail_maskstr;
uniform float S_clouds_detail_mask_tiling;
uniform float S_clouds_detail_mask_tiling_v;
uniform float U_cloudsdiffdetailoffsetu;
uniform float U_cloudsdiffdetailoffsetv;
uniform sampler2D S_cloudmap_map;
uniform float U_cloudcutoff;
uniform sampler2D S_densecloudmap_map;
uniform float U_densecutoff;
uniform float U_anisox;
uniform float U_anisoy;
uniform float U_base_brightness_shift;
uniform float U_base_contrast_shift;
uniform float U_base_saturation_shift;
uniform float U_base_hue_shift;
uniform float U_fdensity;

// As the earthlike stack, except the tiled copy keys on the detail u offset and the shadow
// lookup can drop the base alpha after the first layer.
vec4 cloudLayers(vec2 uv, vec4 base, float cloudW, float denseW, float detailW, bool clearAlpha) {
  float drift = 0.75 * U_cloud_speed * F_time;
  float o = U_cloudsdiffoffsetu;
  vec2 tile = vec2(S_clouds_detail_mask_tiling, S_clouds_detail_mask_tiling_v);
  vec4 c = max(base, cloudW * texture(S_cloudmap_map, uv + vec2(o + drift, -0.1 * drift)));
  if (clearAlpha) c.a = 0.0;
  c = max(c, cloudW * texture(S_cloudmap_map, uv + vec2(o + 0.25 + drift, 0.15 - 0.1 * drift)));
  c = max(c, detailW * cloudW *
                 texture(S_cloudmap_map,
                         vec2(uv.x * tile.x + U_cloudsdiffdetailoffsetu + 0.9 * drift * tile.x,
                              uv.y * tile.y + U_cloudsdiffdetailoffsetv + 0.1 * drift * tile.y)));
  vec2 du = uv + vec2(o, 0.0);
  c = max(c, denseW * texture(S_densecloudmap_map, du + vec2(0.5 + drift, 0.125)));
  c = max(c, denseW * texture(S_densecloudmap_map, du + vec2(-0.15 + drift, -0.25)));
  c = max(c, denseW * texture(S_densecloudmap_map, du + vec2(0.25 + drift, 0.0)));
  return c;
}

void main() {
  float t = F_time;
  float speed = U_cloud_speed;
  float ps = U_polarscale;
  float wl = U_waterlevel;
  vec3 nv = normalize(vNormal);
  vec3 sunDir = V_direction1;
  vec2 detailOffset = vec2(U_diffdetailoffsetu, U_diffdetailoffsetv);
  vec2 baseUv = vUv0 + vec2(U_diffoffsetu, 0.0);

  vec4 albedo = S_diffuse_color;
  if (S_diffuse_bool) albedo = S_diffusestr * texture(S_diffuse_map, baseUv);
  float detailW = S_diffuse_detailstr;
  float cloudDetailW = S_clouds_detail_maskstr;
  if (B_vertexdata0) {
    detailW *= vTint;
    cloudDetailW *= vTint;
  }
  vec2 detailUv = vUv0 * vec2(S_diffuse_detail_tiling, S_diffuse_detail_tiling_v) + detailOffset;
  if (S_diffuse_detail_bool)
    albedo.rgb = (albedo.rgb + detailW * texture(S_diffuse_detail_map, detailUv).rgb) /
                 (S_diffusestr + detailW);
  albedo = saturate(albedo);

  vec4 polar = texture(S_polar_map, vUv0);
  polar.rgb = polar.rgb * U_icecolscale + U_color_plains.rgb * 0.5 * (1.0 - U_icecolscale);
  float ice = smoothstep(ps - 0.01, ps, polar.a + (albedo.r + 0.2) * 0.2 * sign(ps)) *
              -sign(ps - 1.0);
  float ground = 1.0 - ice;

  // Lowlands blend toward the plains colour; their normals flatten with it.
  vec3 surface = albedo.rgb;
  float bump = 1.0;
  if (albedo.r < wl) {
    float w = smoothstep(0.7 * wl, wl, (albedo.r + albedo.g + albedo.b) / 3.0);
    surface = (1.0 - w) * softOverlay(albedo.rgb, U_color_plains.rgb) + w * albedo.rgb;
    bump = 0.75 * w + 0.25;
  }

  vec4 glow = S_color_glow_color;
  vec2 cityUv = vUv0 * vec2(S_citymap_tiling, S_citymap_tiling_v) + detailOffset;
  if (S_citymap_bool) {
    glow = texture(S_color_glow_map, cityUv);
    glow.rgb *= S_color_glowstr;
    vec3 city = 0.5 * (texture(S_citymap_map, 1000.0 * cityUv).rgb +
                       texture(S_citymap_map, 100.0 * cityUv).rgb);
    float cityA = texture(S_citymap_map, 25.0 * cityUv).a * texture(S_citymap_map, 35.0 * cityUv).a;
    float populated = glow.a > 1.0 - U_citylevel ? 1.0 : 0.0;
    glow.rgb *= populated;
    if (populated * cityA > 0.04) surface = softOverlay(surface, city);
  }

  vec3 n = nv;
  if (S_normal_bool) {
    vec3 nt = normalFull(S_normal_map, baseUv, S_normalstr * bump);
    if (S_normal_detail_bool) {
      vec2 nd = normalXY(S_normal_detail_map, detailUv, 1.0) +
                normalXY(S_normal_detail_map,
                         vUv0 * vec2(S_normal_detail_tiling, S_normal_detail_tiling_v) +
                             detailOffset,
                         0.75);
      nt.xy += nd * S_normal_detailstr * bump;
    }
    n = toWorld(nt);
  }
  n = n * ground + nv * ice;
  float gloss = 0.1 * ground;
  surface = surface * ground + polar.rgb * ice * 1.5;

  vec3 v = normalize(vToEye);

  vec4 mask = S_diffuse_color;
  if (S_clouds_mask_bool)
    mask = S_clouds_maskstr * texture(S_clouds_mask_map,
                                      vUv1 + vec2(U_cloudsdiffoffsetu + t * speed, 0.5 * t * speed));
  if (S_clouds_detail_mask_bool) {
    vec2 tile = vec2(S_clouds_detail_mask_tiling, S_clouds_detail_mask_tiling_v);
    vec4 dm = cloudDetailW * cloudDetailW *
              texture(S_clouds_detail_mask_map,
                      vec2(vUv1.x * tile.x + U_cloudsdiffdetailoffsetu + 1.3 * t * speed * tile.x,
                           vUv1.y * tile.y + U_cloudsdiffdetailoffsetv -
                               0.125 * t * speed * tile.y));
    mask.rgb = (mask.rgb + dm.rgb) / (S_clouds_maskstr + cloudDetailW);
  }
  mask = saturate(mask);

  vec4 haze = U_color_atmo;
  haze.a += rim(sunDir, nv, 6.0) * 0.075;
  haze.a += rim(v, nv, 6.0) * pow(U_color_atmo.a, 0.3);
  haze.a -= (1.0 - rim(v, nv, 0.2)) * U_color_atmo.a * 0.5;

  float cloudW = smoothstep(U_cloudcutoff - 0.4, U_cloudcutoff, mask.r);
  float denseW = smoothstep(U_densecutoff - 0.4, U_densecutoff, mask.r);

  vec4 clouds = cloudLayers(vUv1, haze, cloudW, denseW, cloudDetailW, false);
  clouds.rgb = saturate(screen(clouds.rgb, haze.rgb)) + U_color_atmo.rgb;

  float sunN = dot(sunDir, nv);
  vec3 shade = vec3(1.0);
  if (clouds.a > 0.85) shade -= mix(1.0, 0.0, sunN) * 0.65 * clouds.a;
  vec4 caster = cloudLayers(vUv1 + normalize(nv - sunDir).xy * 0.001, haze, cloudW, denseW,
                            cloudDetailW, true);
  if (caster.a > 0.25) shade -= mix(1.0, 0.05, sunN) * 0.75 * caster.a;

  vec2 aniso = vec2(U_anisox, U_anisoy) * (1.0 - gloss);
  surface = saturate(gradeMatrix(U_base_brightness_shift, U_base_contrast_shift,
                                 U_base_saturation_shift, U_base_hue_shift) *
                     vec4(surface, 1.0));
  vec3 col = bodyLight(n, v, gloss, 0.0, surface, aniso);

  vec3 k3 = atmosphereK3();
  col = col * k3.b + col * (1.0 - U_fdensity) + k3 * U_fdensity;
  col += saturate(1.0 - luma(col * 6.0)) * glow.rgb * ground;
  col = amix(col * shade, clouds.rgb * 2.0 * k3 + k3, clouds.a);
  OUT_RT0 = vec4(gammaDecode(saturate(col)), S_diffuse_color.a * F_alphascale);
}
)";

constexpr const char* kGasFs = R"(uniform vec4 S_diffuse_color;
uniform bool S_diffuse_bool;
uniform float S_diffusestr;
uniform sampler2D S_diffuse_map;
uniform float U_diffoffsetu;
uniform float U_diffoffsetv;
uniform float U_diffscalev;
uniform float U_diffspeed;
uniform bool S_diffuse_detail_bool;
uniform sampler2D S_diffuse_detail_map;
uniform float S_diffuse_detailstr;
uniform float S_diffuse_detail_tiling;
uniform float S_diffuse_detail_tiling_v;
uniform float U_diffdetailoffsetu;
uniform float U_diffdetailoffsetv;
uniform float U_diffdetailscalev;
uniform float U_diffdetailspeed;
uniform float U_diffdetailcutoff;
uniform float U_samples;
uniform sampler2D S_color_height_map;
uniform float U_heightscalev;
uniform float U_heightoffsetu;
uniform float U_heightoffsetv;
uniform float U_colorfactor;
uniform float U_smoothnessmax;
uniform bool S_normal_bool;
uniform sampler2D S_normal_map;
uniform float S_normalstr;
uniform bool S_normal_detail_bool;
uniform sampler2D S_normal_detail_map;
uniform float S_normal_detailstr;
uniform float U_anisox;
uniform float U_anisoy;
uniform float U_base_brightness_shift;
uniform float U_base_contrast_shift;
uniform float U_base_saturation_shift;
uniform float U_base_hue_shift;
uniform float U_fdensity;

// Octave i of the detail turbulence: 2.66^i tiling, mirrored on every other octave.
vec2 octaveUv(int i) {
  float fac = pow(2.66, float(i)) * ((i & 1) == 1 ? 1.0 : -1.0);
  return vec2(vUv0.x * S_diffuse_detail_tiling * fac + U_diffdetailoffsetu +
                  0.01 * F_time * U_diffdetailspeed,
              vUv0.y * U_diffdetailscalev * fac * S_diffuse_detail_tiling_v + U_diffdetailoffsetv);
}

void main() {
  float scroll = 0.01 * F_time * U_diffspeed;
  // The band scroll is applied twice; X4's gas giants drift at that rate.
  vec2 bandUv = vec2(vUv0.x + scroll, vUv0.y * U_diffscalev) + vec2(U_diffoffsetu, U_diffoffsetv) +
                vec2(-0.5 + scroll, 0.0);
  vec4 albedo = S_diffuse_color;
  if (S_diffuse_bool) albedo = 2.0 * S_diffusestr * gammaDecode(texture(S_diffuse_map, bandUv));
  float detailW = S_diffuse_detailstr;
  if (B_vertexdata0) detailW *= vTint;

  int octaves = clamp(int(U_samples), 0, 16);
  vec4 detail = vec4(0.0);
  float cut = 1.0;
  if (S_diffuse_detail_bool) {
    for (int i = 1; i <= octaves; ++i) detail += gammaDecode(texture(S_diffuse_detail_map, octaveUv(i)));
    if (U_samples > 0.0) detail.rgb /= U_samples;
    cut = smoothstep(U_diffdetailcutoff - 0.2, U_diffdetailcutoff, detail.r);
    albedo.rgb = max(albedo.rgb, detail.rgb * detailW * cut);
  }
  albedo = saturate(albedo);
  vec4 surface = albedo;

  // color_height is a 32-column gradient atlas; heightoffsetu picks the column.
  vec3 ramp = texture(S_color_height_map, vec2(0.015625 + 0.03125 * U_heightoffsetu,
                                               albedo.r * U_heightscalev + U_heightoffsetv))
                  .rgb;
  surface.rgb = (screen(surface.rgb, ramp) + ramp) * 0.5;
  surface.rgb = surface.rgb * (1.0 - U_colorfactor) + ramp * U_colorfactor;
  float gloss = U_smoothnessmax * (ramp.b + albedo.b) * 0.5;

  vec3 nv = normalize(vNormal);
  vec3 n = nv;
  if (S_normal_bool) {
    vec3 nt = normalFull(S_normal_map, bandUv, S_normalstr);
    if (S_normal_detail_bool) {
      vec2 nd = vec2(0.0);
      for (int i = 1; i <= octaves; ++i) nd += normalXY(S_normal_detail_map, octaveUv(i), 1.0);
      nt.xy += nd * S_normal_detailstr;
      nt = normalize(nt);
    }
    // Bright bands bend the normal away from the limb, which shapes the terminator.
    float bend = sign(nv.x) * -0.25 * abs(nv.x);
    nt.x += surface.r * bend;
    nt.x += detail.r * bend * cut;
    n = toWorld(normalize(nt));
  }

  vec3 v = normalize(vToEye);
  surface.rgb = saturate(gradeMatrix(U_base_brightness_shift, U_base_contrast_shift,
                                     U_base_saturation_shift, U_base_hue_shift) *
                         vec4(surface.rgb, 1.0));
  vec2 aniso = vec2(U_anisox, U_anisoy) * (1.0 - gloss);
  vec3 col = bodyLight(n, v, gloss, 0.1, surface.rgb, aniso);
  col += surface.rgb * atmosphereK3() * U_fdensity;
  vec4 o = max(vec4(0.0), vec4(col, surface.a * F_alphascale));
  OUT_RT0 = vec4(gammaDecode(o.rgb), o.a);
}
)";

constexpr const char* kEarthlikeMvFs = R"(uniform vec4 S_diffuse_color;
uniform bool S_diffuse_bool;
uniform float S_diffusestr;
uniform sampler2D S_diffuse_map;
uniform float U_diffoffsetu;
uniform bool S_diffuse_detail_bool;
uniform sampler2D S_diffuse_detail_map;
uniform float S_diffuse_detailstr;
uniform float S_diffuse_detail_tiling;
uniform float S_diffuse_detail_tiling_v;
uniform float U_diffdetailoffsetu;
uniform float U_diffdetailoffsetv;
uniform sampler2D S_polar_map;
uniform float U_polarscale;
uniform vec4 U_color_water;
uniform float U_waterlevel;
uniform bool S_color_height_bool;
uniform sampler2D S_color_height_map;
uniform float S_color_heightstr;
uniform bool S_color_climate_bool;
uniform sampler2D S_color_climate_map;
uniform float S_color_climatestr;
uniform bool S_citymap_bool;
uniform sampler2D S_citymap_map;
uniform float S_citymap_tiling;
uniform float S_citymap_tiling_v;
uniform float U_citylevel;
uniform bool S_color_glow_bool;
uniform sampler2D S_color_glow_map;
uniform vec4 S_color_glow_color;
uniform float S_color_glowstr;
uniform bool S_normal_bool;
uniform sampler2D S_normal_map;
uniform float S_normalstr;
uniform bool S_normal_detail_bool;
uniform sampler2D S_normal_detail_map;
uniform float S_normal_detailstr;
uniform float S_normal_detail_tiling;
uniform float S_normal_detail_tiling_v;
uniform vec4 U_color_atmo;
uniform float U_cloud_speed;
uniform float U_cloud_colorfactor;
uniform float U_cloudsdiffoffsetu;
uniform bool S_cloudmap_bool;
uniform sampler2D S_cloudmap_map;
uniform float S_cloudmapstr;
uniform bool S_uvoffset_bool;
uniform sampler2D S_uvoffset_map;
uniform float S_uvoffsetstr;
uniform float U_blur;
uniform bool S_moisture_bool;
uniform sampler2D S_moisture_map;
uniform float S_moisturestr;
uniform float U_cloudiness;
uniform float U_is_lava;
uniform float U_lavalevel;
uniform bool S_water_bool;
uniform sampler2D S_water_map;
uniform float S_water_tiling;
uniform float S_water_tiling_v;
uniform float U_anisox;
uniform float U_anisoy;
uniform float U_base_brightness_shift;
uniform float U_base_contrast_shift;
uniform float U_base_saturation_shift;
uniform float U_base_hue_shift;
uniform float U_fdensity;

void main() {
  float t = F_time;
  float ps = U_polarscale;
  float wl = U_waterlevel;
  vec3 nr = vNormal;  // several terms deliberately use the unnormalized interpolant
  vec3 nv = normalize(nr);
  vec3 sunDir = V_direction1;
  vec2 detailOffset = vec2(U_diffdetailoffsetu, U_diffdetailoffsetv);
  vec2 baseUv = vUv0 + vec2(U_diffoffsetu, 0.0);
  float gloss = 0.1;
  float metal = 0.0;

  vec4 albedo = S_diffuse_color;
#ifdef MV_UN
  if (S_diffuse_bool) albedo = S_diffusestr * textureLod(S_diffuse_map, baseUv, 0.0);
#else
  if (S_diffuse_bool) albedo = S_diffusestr * texture(S_diffuse_map, baseUv);
#endif
  float detailW = S_diffuse_detailstr;
  if (B_vertexdata0) detailW *= vTint;
  vec2 detailUv = vUv0 * vec2(S_diffuse_detail_tiling, S_diffuse_detail_tiling_v) + detailOffset;
  if (S_diffuse_detail_bool)
    albedo.rgb = (albedo.rgb + detailW * texture(S_diffuse_detail_map, detailUv).rgb) /
                 (S_diffusestr + detailW);
  albedo = saturate(albedo);

  vec4 polar = texture(S_polar_map, vUv0);
#ifdef MV_UN
  float elev = albedo.a;  // the _un diffuse carries its heightfield in alpha
  float ice = smoothstep(ps - 0.01, ps, pow(polar.a, 2.2) + 0.5 * elev * sign(ps)) *
              -sign(ps - 1.0);
#else
  float elev = albedo.r;
  float ice = smoothstep(ps - 0.01, ps, polar.a + (elev + 0.2) * 0.2 * sign(ps)) *
              -sign(ps - 1.0);
#endif
  float ground = 1.0 - ice;
  vec4 glow = S_color_glow_color;
  vec3 n = nv;
  vec3 surface = polar.rgb * ice * 1.5;
  vec4 water = vec4(U_color_water.rgb, 1.0);

#ifdef MV_UN
  float lavaLevel = U_lavalevel;
  float lavaW = 0.0;
  if (elev < lavaLevel) {
    if (S_water_bool && U_is_lava > 0.0) {
      water = 0.5 * texture(S_water_map, vUv0 * vec2(S_water_tiling, S_water_tiling_v));
      water.rgb += pow(smoothstep(lavaLevel - 0.1, lavaLevel, elev), 2.5) *
                   vec3(0.1251, 0.251, 0.1) * 0.5;
    }
    surface += max(water.rgb, water.rgb * (2.01 - lavaLevel + 1.52 * elev));
    surface += max(vec3(0.0), (elev - 0.85 * lavaLevel) * vec3(1.0, 1.0, 0.5));
    gloss = 0.85 * ground;
    metal = 0.7;
    lavaW = 1.0 - smoothstep(lavaLevel - 0.2, lavaLevel, elev);
  }
#endif

  if (elev < wl) {
    surface += max(water.rgb, water.rgb * (2.01 - wl + 1.52 * elev));
    surface += max(vec3(0.0), (elev - 0.85 * wl) * vec3(1.0, 1.0, 0.5));
    gloss = 0.85 * ground;
    metal = 0.7;
  } else {
    surface += albedo.rgb;
#ifdef MV_UN
    if (S_color_height_bool) {
      vec3 height = texture(S_color_height_map, vec2((elev - wl) / (1.0 - wl), 0.5)).rgb;
      surface = surface * (1.0 - S_color_heightstr) + S_color_heightstr * surface * height;
    }
    if (S_color_climate_bool) {
      vec3 climate = texture(S_color_climate_map,
                             vec2(vUv0.x, vUv0.y + sign(vUv0.y - 0.5) * (0.5 - ps * 0.5)))
                         .rgb;
      surface = surface * (1.0 - S_color_climatestr) + S_color_climatestr * surface * climate;
    }
    float peak = elev;
#else
    vec3 height = texture(S_color_height_map, vec2((elev - wl) / (1.0 - wl), 0.5)).rgb;
    surface *= height;
    surface = climateBlend(
        surface, texture(S_color_climate_map,
                         vec2(2.0 * vUv0.x, vUv0.y + sign(vUv0.y - 0.5) * (0.5 - ps * 0.5)))
                     .rgb);
    float peak = height.b;
#endif

    vec2 cityUv = vUv0 * vec2(S_citymap_tiling, S_citymap_tiling_v) + detailOffset;
    if (S_citymap_bool) {
      glow = texture(S_color_glow_map, cityUv);
      glow.rgb *= S_color_glowstr;
      vec3 city = 0.5 * (texture(S_citymap_map, 1000.0 * cityUv).rgb +
                         texture(S_citymap_map, 100.0 * cityUv).rgb);
      float cityA =
          texture(S_citymap_map, 25.0 * cityUv).a * texture(S_citymap_map, 35.0 * cityUv).a;
      float populated = glow.a > 1.0 - U_citylevel ? 1.0 : 0.0;
      glow.rgb *= populated;
      if (populated * cityA > 0.04) surface = softOverlay(surface, city);
    }
#ifdef MV_UN
    else if (S_color_glow_bool) {
      glow = texture(S_color_glow_map, vUv0);
      glow.rgb *= S_color_glowstr;
    }
#endif

    if (S_normal_bool) {
      vec3 nt = normalFull(S_normal_map, baseUv, S_normalstr);
      if (S_normal_detail_bool) {
        vec2 nd = normalXY(S_normal_detail_map, detailUv, S_normalstr) +
                  normalXY(S_normal_detail_map,
                           vUv0 * vec2(S_normal_detail_tiling, S_normal_detail_tiling_v) +
                               detailOffset,
                           0.75);
        nt.xy += nd * S_normal_detailstr;
      }
      n = toWorld(normalize(nt));
    }
    n = normalize(0.5 * ground * n + 0.5 * ice * nv);
    ice = saturate(ice + smoothstep(0.8, 0.99, peak));
    ground = 1.0 - ice;
    gloss = 0.1 * ground;
  }
#ifdef MV_UN
  if (U_is_lava < 1.0) surface = surface * ground + polar.rgb * ice * 1.5;
#else
  surface = surface * ground + polar.rgb * ice * 1.5;
#endif

  vec3 v = normalize(vToEye);

  // Three cloud octaves advected by the motion map, thinned by moisture and cloudiness.
  vec4 clouds = U_color_atmo;
  vec2 flow = vUv1;
  if (S_uvoffset_bool) {
    vec2 d = 2.0 * (textureLod(S_uvoffset_map, vUv1, float(int(U_blur))).rg - 0.5);
    flow += S_uvoffsetstr * d + vec2(U_cloudsdiffoffsetu + t * U_cloud_speed, 0.0);
  }
  vec3 moisture = vec3(1.0);
  if (S_moisture_bool) moisture = S_moisturestr * texture(S_moisture_map, vUv0).rgb;
  vec3 octW = vec3(0.0);
  if (S_cloudmap_bool) {
    float c = U_cloudiness;
    octW = vec3(c, pow(c, 1.5), c * c);
    vec4 fine = S_cloudmapstr * texture(S_cloudmap_map, 2.0 * flow);
    fine.a *= moisture.r * octW.x;
    vec4 finest = S_cloudmapstr * texture(S_cloudmap_map, 4.0 * flow);
    finest.a *= moisture.g * octW.y;
    vec4 coarse = S_cloudmapstr * texture(S_cloudmap_map, flow);
    coarse.a *= moisture.b * octW.z;
    clouds = over(over(coarse, finest), fine);
    clouds.a = saturate(1.25 * (2.5 * clouds.a - 0.5) + 0.5);
  }

  vec4 atmo = vec4(gammaDecode(U_color_atmo.rgb), U_color_atmo.a);
  vec4 haze = atmo;
  haze.a += rim(sunDir, nr, 6.0) * 0.075;
#ifndef MV_UN
  haze.a += rim(v, nr, 6.0) * pow(atmo.a, 0.3);
  haze.a -= (1.0 - rim(v, nr, 0.2)) * atmo.a * 0.5;
#endif
  clouds = max(haze, clouds);
  clouds.rgb = amix(clouds.rgb, atmo.rgb, U_cloud_colorfactor);

  vec3 shade = vec3(1.0);
  if (clouds.a < 1.0) {
    vec2 ds = normalize(nr - sunDir).xy * vec2(0.001, -0.001);
    vec4 caster = texture(S_cloudmap_map, flow - ds) * moisture.b * octW.z;
    caster = over(caster, texture(S_cloudmap_map, 4.0 * flow - ds) * moisture.g * octW.y);
    caster = over(caster, texture(S_cloudmap_map, 2.0 * flow - ds) * moisture.r * octW.x);
    caster.a = saturate(1.25 * (1.5 * caster.a - 0.5) + 0.5);
#ifdef MV_UN
    const float kShadowThreshold = 0.5;
#else
    const float kShadowThreshold = 0.4;
#endif
    if (caster.a > kShadowThreshold) shade -= mix(0.990, 0.5, saturate(dot(sunDir, nr)));
  }

  vec2 aniso = vec2(U_anisox, U_anisoy) * (1.0 - gloss);
  surface = saturate(gradeMatrix(U_base_brightness_shift, U_base_contrast_shift,
                                 U_base_saturation_shift, U_base_hue_shift) *
                     vec4(surface, 1.0));
  vec3 col = bodyLight(n, v, gloss, metal, surface, aniso);

  vec3 kv = vInscatter + 0.25 * vExtinction;
#ifdef MV_UN
  if (U_is_lava > 0.0) {
    float lava = 3.60 * lavaW * U_is_lava;
    col += water.rgb * lava;
    clouds.rgb += water.rgb * clouds.rgb * lava;
    kv += clouds.rgb * 0.10 * lavaW * clouds.a * (1.0 - saturate(dot(sunDir, nr)));
  }
#endif
  vec3 k3 = 1.0 - exp(-kv);
#ifdef MV_UN
  col = col * k3.b + col * (1.0 - U_fdensity) + k3.b * U_color_atmo.rgb * U_fdensity;
#else
  col = col * k3.b + col * (1.0 - U_fdensity);
#endif
  col += k3 * U_fdensity;
#ifdef MV_UN
  col += saturate(pow(saturate(dot(nr, -sunDir) - 0.1), 0.5)) * glow.rgb * ground;
#else
  col += saturate(1.0 - luma(6.0 * col)) * glow.rgb * ground;
#endif
  col = amix(col * shade, clouds.rgb * 2.0 * k3 + k3, clouds.a);
  vec4 o = max(vec4(0.0), vec4(col, S_diffuse_color.a * F_alphascale));
  OUT_RT0 = vec4(gammaDecode(o.rgb), o.a);  // HDR: no clamp to 1
}
)";

constexpr const char* kGasMvFs = R"(uniform bool S_diffuse_bool;
uniform sampler2D S_diffuse_map;
uniform bool S_diffuse_detail_bool;
uniform sampler2D S_diffuse_detail_map;
uniform float S_diffuse_detailstr;
uniform float S_diffuse_detail_tiling;
uniform float S_diffuse_detail_tiling_v;
uniform float U_diffdetailoffsetu;
uniform float U_diffdetailoffsetv;
uniform float U_diffdetailscalev;
uniform float U_diffdetailcutoff;
uniform float U_samples;
uniform bool S_uvoffset_bool;
uniform sampler2D S_uvoffset_map;
uniform float S_uvoffsetstr;
uniform float U_blur;
uniform sampler2D S_color_height_map;
uniform float U_heightscalev;
uniform float U_heightoffsetu;
uniform float U_heightoffsetv;
uniform float U_colorfactor;
uniform float U_smoothnessmax;
uniform bool S_normal_bool;
uniform sampler2D S_normal_map;
uniform float S_normalstr;
uniform bool S_normal_detail_bool;
uniform sampler2D S_normal_detail_map;
uniform float S_normal_detailstr;
uniform float U_cloudoffsetx;
uniform float U_cloudoffsety;
uniform float U_cloudshadowpower;
uniform float U_anisox;
uniform float U_anisoy;
uniform float U_base_brightness_shift;
uniform float U_base_contrast_shift;
uniform float U_base_saturation_shift;
uniform float U_base_hue_shift;
uniform float U_fdensity;

// Motion-map advection: the map stores a signed flow in rg; v runs the other way.
vec2 advect(vec2 uv, vec2 motionUv, float lod) {
  vec2 d = 2.0 * (textureLod(S_uvoffset_map, motionUv, lod).rg - 0.5);
  return uv + S_uvoffsetstr * vec2(d.x, -d.y);
}

vec2 detailOctave(int i) {
  float f = exp2(float(i));
  return vec2(vUv0.x * f * S_diffuse_detail_tiling + U_diffdetailoffsetu,
              vUv0.y * f * S_diffuse_detail_tiling_v * U_diffdetailscalev + U_diffdetailoffsetv);
}

void main() {
  float mip = float(int(U_blur));
  int layers = clamp(int(U_samples), 0, 16);
  bool moving = S_uvoffset_bool;
  float lod = moving ? mip : 0.0;
#ifdef MV_UN
  float bandLod = 0.0;
#else
  float bandLod = lod;
#endif

  // Stack of ever deeper, darker band layers; layer 1's coverage is the cloud top depth.
  vec4 bands = vec4(0.0);
  float topDepth = 0.0;
  if (S_diffuse_bool) {
    for (int i = 1; i <= layers; ++i) {
      vec2 uv = moving ? advect(vUv0 * float(i), vUv1, mip) : vUv0;
      vec4 layer = textureLod(S_diffuse_map, uv, bandLod);
      layer.rgb *= 1.0 - 0.2 * float(i);
      bands = over(bands, layer);
      if (i == 1) topDepth = 1.0 - layer.a;
    }
  }
  float detailW = S_diffuse_detailstr;
  if (B_vertexdata0) detailW *= vTint;
  if (S_diffuse_detail_bool) {
    vec4 detail = vec4(0.0);
    for (int i = 1; i <= layers; ++i) {
      vec2 uv = moving ? advect(detailOctave(i), vUv1, mip) : detailOctave(i);
      detail += textureLod(S_diffuse_detail_map, uv, lod);
    }
    if (U_samples > 0.0) detail.rgb /= U_samples;
    float cut = smoothstep(U_diffdetailcutoff - 0.2, U_diffdetailcutoff, detail.r);
    bands.rgb = max(bands.rgb, detail.rgb * detailW * cut);
  }
  bands = saturate(bands);
  vec4 surface = bands;

  vec3 ramp = texture(S_color_height_map, vec2(0.015625 + 0.03125 * U_heightoffsetu,
                                               bands.r * U_heightscalev + U_heightoffsetv))
                  .rgb;
  surface.rgb = (screen(surface.rgb, ramp) + ramp) * 0.5;
  surface.rgb = surface.rgb * (1.0 - U_colorfactor) + ramp * U_colorfactor;
  float gloss = U_smoothnessmax * (ramp.b + bands.b) * 0.5;

  vec3 nr = vNormal;
  vec3 n = nr;
  if (S_normal_bool) {
    vec3 nt = vec3(0.0);
    for (int i = 1; i <= layers; ++i) {
      vec2 uv = moving ? advect(vUv0 * float(i), vUv1, mip) : vUv0;
      nt += 2.0 * textureLod(S_normal_map, uv, lod).xyz - 1.0;
    }
    nt = normalize(nt / U_samples);
    nt.xy *= S_normalstr;
    if (S_normal_detail_bool) {
      vec2 nd = vec2(0.0);
      for (int i = 1; i <= layers; ++i) {
        vec2 uv = moving ? advect(detailOctave(i), vUv1, mip) : detailOctave(i);
        nd += (2.0 * textureLod(S_normal_detail_map, uv, lod).rgb - 1.0).xy;
      }
      nt.xy += nd * S_normal_detailstr;
      nt = normalize(nt);
    }
    n = toWorld(nt);
  }

  vec3 v = normalize(vToEye);

  // Band self-shadow: march the top layer toward the sun; the z flip reads display axes the
  // way X4's left-handed world does.
  float lit = 1.0;
  vec3 sd = (normalize(V_direction1) - n) * vec3(1.0, 1.0, -1.0);
  if (sd.z < 0.0) sd.y = 1.0 - sd.y;
  if (S_diffuse_bool) {
    for (int i = 1; i <= 3; ++i) {
      for (int j = 1; j <= 3; ++j) {
        vec2 o = vec2(U_cloudoffsetx * float(i), -U_cloudoffsety * float(j)) * 0.01 * sd.xz;
        vec2 uv = moving ? advect(vUv0, vUv1 + o, mip) : vUv0;
        float density = 1.0 - textureLod(S_diffuse_map, uv, lod).a;
        if (topDepth > density + 0.1) lit -= 0.005;
      }
    }
  }
  surface.rgb = saturate(gradeMatrix(U_base_brightness_shift * pow(lit, U_cloudshadowpower),
                                     U_base_contrast_shift, U_base_saturation_shift,
                                     U_base_hue_shift) *
                         vec4(surface.rgb, 1.0));

  vec2 aniso = vec2(U_anisox, U_anisoy) * (1.0 - gloss);
  vec3 col = bodyLight(n, v, gloss, 0.1, surface.rgb, aniso);
  col += surface.rgb * atmosphereK3() * U_fdensity;

  // Only reaches target alpha (bodies draw unblended).
  float f = abs(dot(v, nr));
  float alpha = f < 0.075 ? f / 0.075 - saturate(max(0.0, 10.0 * (topDepth - 0.75))) *
                                            pow(f / 0.35, 0.7)
                          : 1.0;
  vec4 o = max(vec4(0.0), vec4(col, alpha * F_alphascale));
  OUT_RT0 = vec4(gammaDecode(o.rgb), o.a);
}
)";

constexpr const char* kSunFs = R"(uniform bool S_diffuse_bool;
uniform sampler2D S_diffuse_map;
uniform float S_diffusestr;
uniform float U_diffoffsetu;
uniform float U_diffoffsetv;
uniform float U_diffscalev;
uniform float U_diffspeed;
uniform float U_basesamples;
uniform bool S_diffuse_detail_bool;
uniform sampler2D S_diffuse_detail_map;
uniform float S_diffuse_detailstr;
uniform float S_diffuse_detail_tiling;
uniform float S_diffuse_detail_tiling_v;
uniform float U_diffdetailoffsetu;
uniform float U_diffdetailoffsetv;
uniform float U_diffdetailscalev;
uniform float U_diffdetailspeed;
uniform float U_diffdetailcutoff;
uniform float U_samples;
uniform bool S_color_height_bool;
uniform sampler2D S_color_height_map;
uniform float S_color_heightstr;
uniform float U_heightscalev;
uniform float U_heightoffsetu;
uniform float U_heightoffsetv;
uniform float U_pulsestrength;
uniform bool S_normal_bool;
uniform sampler2D S_normal_map;
uniform float S_normalstr;
uniform float U_base_brightness_shift;
uniform float U_base_contrast_shift;
uniform float U_base_saturation_shift;
uniform float U_base_hue_shift;

void main() {
  float t = F_time;
  vec4 base = vec4(0.0, 0.0, 0.0, 1.0);
  float detailW = S_diffuse_detailstr;
  if (B_vertexdata0) detailW *= vTint;

  if (S_diffuse_bool) {
    vec4 acc = vec4(0.0);
    int n = clamp(int(U_basesamples), 0, 16);
    for (int i = 1; i <= n; ++i) {
      float k = float(i);
      acc += texture(S_diffuse_map, vec2(vUv0.x * k + U_diffoffsetu + 0.05 * t * U_diffspeed,
                                         vUv0.y * U_diffscalev * k + U_diffoffsetv));
    }
    base.rgb = S_diffusestr * acc.rgb / (U_basesamples + 0.01);
  }

  // Counter-rotating detail octaves: the scroll uses the sign of the NEXT octave.
  if (S_diffuse_detail_bool) {
    vec4 detail = vec4(1.0);
    float dir = 1.0;
    int n = clamp(int(U_samples), 0, 16);
    for (int i = 1; i <= n; ++i) {
      float fac = dir * float(i);
      dir = -dir;
      vec2 uv = vec2(vUv0.x * S_diffuse_detail_tiling * fac + U_diffdetailoffsetu +
                         0.05 * t * U_diffdetailspeed * dir,
                     vUv0.y * U_diffdetailscalev * fac * S_diffuse_detail_tiling_v +
                         U_diffdetailoffsetv);
      detail *= 2.0 * texture(S_diffuse_detail_map, uv);
    }
    float cut =
        smoothstep(max(0.0, U_diffdetailcutoff - 0.2), U_diffdetailcutoff + 0.001, detail.r);
    base.rgb *= detail.rgb * detailW * cut;
    base.a = 1.0;
  }
  base = saturate(base);
  vec4 surface = base;

  // color_height is an 8-column heat gradient; green drives it, pulsing over time.
  if (S_color_height_bool) {
    float heat = (base.g + sin(0.5 * t) * U_pulsestrength) * U_heightscalev + U_heightoffsetv;
    surface.rgb = S_color_heightstr *
                  texture(S_color_height_map,
                          vec2(0.0625 + 0.125 * U_heightoffsetu,
                               1.0 - max(0.01, min(0.990 * U_heightscalev, heat))))
                      .rgb;
  }

  // The sun's normal_map slot holds a colour texture: granulation, three mirrored octaves.
  if (S_normal_bool) {
    vec3 grain = vec3(1.0);
    float dir = 1.0;
    for (int i = 1; i <= 3; ++i) {
      float fac = 30.0 * dir * float(i);
      dir = -dir;
      grain *= texture(S_normal_map, vec2(vUv0.x * fac + U_diffoffsetu + t * U_diffspeed * dir,
                                          vUv0.y * U_diffscalev * fac + U_diffoffsetv))
                   .rgb;
    }
    surface.rgb += 0.1 * grain * S_normalstr;
  }

  surface.rgb =
      saturate(gradeMatrix(U_base_brightness_shift * F_globallightscale, U_base_contrast_shift,
                           U_base_saturation_shift, U_base_hue_shift) *
               vec4(surface.rgb, 1.0));
  vec4 o = max(vec4(0.0), vec4(surface.rgb, surface.a * F_alphascale));
  OUT_RT0 = vec4(2.0 * gammaDecode(o.rgb), o.a);
}
)";

// Sky-from-space shell (O'Neil, GPU Gems 2 ch. 16), Rayleigh only, three samples.
constexpr const char* kAtmosphereVs = R"(#version 330
in vec3 vertexPosition;
uniform mat4 mvp;
uniform mat4 worldRot;
uniform vec3 worldPos;
uniform vec3 V_direction1;
uniform float U_wavelength_red;
uniform float U_wavelength_green;
uniform float U_wavelength_blue;
uniform float U_fouterradius;
uniform float U_finnerradius;
uniform float U_m_kr;
uniform float U_m_km;
uniform float U_m_esun;
uniform float U_fscaledepth;
out vec3 vRayleigh;
out vec3 vToEye;

const float kPi = 3.14159265359;

// The shell keeps the book's fixed 0.25 here even when the material's scale depth differs.
float opticalDepth(float cosAngle) {
  float x = 1.0 - cosAngle;
  return 0.25 * exp(-0.00287 + x * (0.459 + x * (3.83 + x * (-6.80 + x * 5.25))));
}

void main() {
  vec3 rel = mat3(worldRot) * vertexPosition;
  vec3 eye = -worldPos;
  float outer = U_fouterradius;
  float inner = U_finnerradius;
  float depth = U_fscaledepth;

  vec3 dir = rel - eye;
  float len = length(dir);
  dir /= len;
  float b = 2.0 * dot(eye, dir);
  float c = dot(eye, eye) - outer * outer;
  float entry = 0.5 * (-b - sqrt(max(0.0, b * b - 4.0 * c)));
  vec3 start = eye + dir * entry;
  len -= entry;

  vec3 invWave4 = 1.0 / pow(vec3(U_wavelength_red, U_wavelength_green, U_wavelength_blue),
                            vec3(4.0));
  float scale = 1.0 / (outer - inner);
  float scaleOverDepth = scale / depth;
  float startOffset = exp(-1.0 / depth) * opticalDepth(dot(dir, start) / outer);
  vec3 extinctK = invWave4 * (U_m_kr * 4.0 * kPi) + U_m_km * 4.0 * kPi;

  float segLen = len / 3.0;
  vec3 p = start + dir * (segLen * 0.5);
  vec3 sum = vec3(0.0);
  for (int i = 0; i < 3; ++i) {
    float h = length(p);
    float density = exp(scaleOverDepth * (inner - h));
    float scatter = startOffset + density * (opticalDepth(dot(V_direction1, p) / h) -
                                             opticalDepth(dot(dir, p) / h));
    vec3 att = clamp(exp(-scatter * extinctK), 0.0, 1.0);
    sum += att * density * segLen * scale;
    p += dir * segLen;
  }
  vRayleigh = sum * invWave4 * U_m_kr * U_m_esun;
  vToEye = eye - rel;
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

constexpr const char* kAtmosphereFs = R"(#version 330
in vec3 vRayleigh;
in vec3 vToEye;
out vec4 OUT_RT0;
uniform vec3 V_direction1;
uniform float U_fdensity;
uniform vec4 U_color_atmo;

void main() {
  float c = dot(V_direction1, vToEye) / length(vToEye);
  vec3 col = (0.75 + 0.75 * c * c) * vRayleigh;
  col = (1.0 - exp(-4.0 * col)) * U_fdensity;
  // Tinted shells keep only the blue scatter, as a mask for the authored colour.
  if (U_color_atmo.r < 1.0) col = U_color_atmo.rgb * col.b * col.b;
  OUT_RT0 = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)";

constexpr const char* kRingVs = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec2 vertexTexCoord;
in vec2 vertexTexCoord2;
in vec4 vertexBoneWeights;  // UV maps 2 and 3 (xmf_model.hpp)
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 worldRot;
uniform vec3 worldPos;
uniform vec4 uvLinear[4];
uniform vec2 uvOffset[4];
uniform float U_camera_fade_range_near_start;
uniform float U_camera_fade_range_near_end;
uniform float U_camera_fade_range_far_start;
uniform float U_camera_fade_range_far_end;
uniform float U_angle_fade_speed;
uniform float U_angle_fade_offset;
out vec2 IO_uv0;
out vec2 IO_uv1;
out vec2 IO_uv2;
out vec2 IO_uv3;
out vec4 IO_color;
out float IO_Fade;

void main() {
  mat3 rot = mat3(worldRot);
  vec3 toVertex = worldPos + rot * vertexPosition;
  IO_uv0 = mat2(uvLinear[0]) * vertexTexCoord + uvOffset[0];
  IO_uv1 = mat2(uvLinear[1]) * vertexTexCoord2 + uvOffset[1];
  IO_uv2 = mat2(uvLinear[2]) * vertexBoneWeights.xy + uvOffset[2];
  IO_uv3 = mat2(uvLinear[3]) * vertexBoneWeights.zw + uvOffset[3];
  IO_color = vec4(pow(vertexColor.rgb + vec3(1e-5), vec3(2.2)), vertexColor.a);
  float dist = length(toVertex);
  float facing = abs(dot(normalize(rot * vertexNormal), normalize(toVertex)));
  IO_Fade = smoothstep(U_camera_fade_range_near_start, U_camera_fade_range_near_end, dist) *
            (1.0 - smoothstep(U_camera_fade_range_far_start, U_camera_fade_range_far_end, dist)) *
            clamp(pow(facing, U_angle_fade_speed) - U_angle_fade_offset, 0.0, 1.0);
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

constexpr const char* kAuroraVs = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec2 vertexTexCoord;
in vec2 vertexTexCoord2;
in vec4 vertexBoneWeights;  // UV maps 2 and 3 (xmf_model.hpp)
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 worldRot;
uniform vec3 worldPos;
uniform vec4 uvLinear[4];
uniform vec2 uvOffset[4];
uniform float U_camera_fade_near_start;
uniform float U_camera_fade_near_stop;
uniform float U_camera_fade_far_start;
uniform float U_camera_fade_far_stop;
uniform float U_angle_fade;
out vec2 IO_uv0;
out vec2 IO_uv1;
out vec2 IO_uv2;
out vec2 IO_uv3;
out vec4 IO_color;
out float IO_Fade;

// Linear 0..1 ramp over [lo, hi].
float ramp(float x, float lo, float hi) { return clamp(x - lo, 0.0, hi - lo) / (hi - lo); }

void main() {
  mat3 rot = mat3(worldRot);
  vec3 toVertex = worldPos + rot * vertexPosition;
  IO_uv0 = mat2(uvLinear[0]) * vertexTexCoord + uvOffset[0];
  IO_uv1 = mat2(uvLinear[1]) * vertexTexCoord2 + uvOffset[1];
  IO_uv2 = mat2(uvLinear[2]) * vertexBoneWeights.xy + uvOffset[2];
  IO_uv3 = mat2(uvLinear[3]) * vertexBoneWeights.zw + uvOffset[3];
  IO_color = vec4(pow(vertexColor.rgb + vec3(1e-5), vec3(2.2)), vertexColor.a);
  float dist = length(toVertex);
  // The normal is deliberately left unnormalized here (unlike the ring fade).
  float facing = abs(dot(rot * vertexNormal, normalize(toVertex)));
  IO_Fade = ramp(dist, U_camera_fade_near_start, U_camera_fade_near_stop) *
            (1.0 - ramp(dist, U_camera_fade_far_start, U_camera_fade_far_stop)) *
            clamp(facing - U_angle_fade / 90.0, 0.0, 1.0);
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

constexpr const char* kComplexUnlitFsHeader = R"(#version 330
#extension GL_ARB_sample_shading : require
in vec2 IO_uv0;
in vec2 IO_uv1;
in vec2 IO_uv2;
in vec2 IO_uv3;
in vec4 IO_color;
in float IO_Fade;
out vec4 OUT_RT0;
uniform vec4 S_diffuse_color;
uniform bool S_diffuse_bool;
uniform float S_diffusestr;
uniform sampler2D S_diffuse_map;
uniform bool S_diffuse2_bool;
uniform float S_diffuse2str;
uniform sampler2D S_diffuse2_map;
uniform bool S_diffuse3_bool;
uniform float S_diffuse3str;
uniform sampler2D S_diffuse3_map;
uniform bool S_alpha1_bool;
uniform float S_alpha1str;
uniform sampler2D S_alpha1_map;
uniform bool S_alpha2_bool;
uniform float S_alpha2str;
uniform sampler2D S_alpha2_map;
uniform bool S_alpha3_bool;
uniform float S_alpha3str;
uniform sampler2D S_alpha3_map;
uniform float U_dblend1;
uniform float U_dblend2;
uniform bool B_vertexdata0;
uniform vec4 U_color_emissive;
uniform float U_base_brightness_shift;
uniform float U_base_contrast_shift;
uniform float U_base_saturation_shift;
uniform float U_base_hue_shift;
uniform float U_useralphascale;
uniform float F_alphascale;
)";

// Shared by planets (rings, flares, auroras) and the station's complex-unlit program; the
// station prepends macros for the uniform names and defines STATION_HULL.
constexpr const char* kComplexUnlitFsMain = R"(
// Layer blend modes 2..6; anything else multiplies. The strength scales rgb a second time.
vec4 blendLayer(vec4 base, vec4 layer, float strength, int mode) {
  vec3 l = strength * layer.rgb;
  if (mode == 2) return base + strength * layer;
  if (mode == 3) return base - strength * layer;
  if (mode == 4) return vec4(1.0 - (1.0 - base.rgb) * (1.0 - l), 1.0 - (1.0 - base.a) * (1.0 - layer.a));
  if (mode == 5) return vec4(softOverlay(base.rgb, l), softOverlay(base.a, layer.a));
  if (mode == 6) return vec4((base.rgb + l) * 0.5, (base.a + layer.a) * 0.5);
  return base * (strength * layer);
}

#ifdef STATION_HULL
// Also called by the station's lightcone body.
float viewDepth(float d) {
  float n = depthRange.x;
  float f = depthRange.y;
  return 2.0 * n * f / (f + n - (2.0 * d - 1.0) * (f - n));
}
float softAtt(float lo, float hi, float z) { return saturate((z - lo) / (hi - lo + 1e-5)); }

const int kDitherRank[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);

// Screen-door LOD dissolve per MSAA sample; a fading pair uses complementary masks.
int dissolveMask() {
  if (opaqueUnlit != 1 || lodFade >= 1.0) return -1;
  ivec2 px = ivec2(gl_FragCoord.xy);
  int rank = kDitherRank[(px.x & 3) + 4 * (px.y & 3)];
  int samples = min(gl_NumSamples, 16);
  int mask = 0;
  for (int i = 0; i < samples; ++i)
    if ((float(i * 16 + rank) + 0.5) / float(16 * samples) < lodFade) mask |= 1 << i;
  if (lodFadeInvert == 1) mask = ~mask & ((1 << samples) - 1);
  return mask;
}
#endif

void main() {
#ifdef STATION_HULL
  int coverage = dissolveMask();
  if (coverage == 0) discard;
  gl_SampleMask[0] = coverage;  // an unwritten mask covers nothing
  if (lightcone != 0) {
    lightconeMain();
    return;
  }
#endif
  vec4 base = S_diffuse_color;
  if (S_diffuse_bool) base = S_diffusestr * texture(S_diffuse_map, IO_uv0);
  if (S_diffuse2_bool)
    base = blendLayer(base, S_diffuse2str * texture(S_diffuse2_map, IO_uv1), S_diffuse2str,
                      int(U_dblend1));
  if (S_diffuse3_bool)
    base = blendLayer(base, S_diffuse3str * texture(S_diffuse3_map, IO_uv2), S_diffuse3str,
                      int(U_dblend2));
  if (B_vertexdata0) base.rgb *= IO_color.rgb;

  // Masks carry their data in green.
  float a1 = S_alpha1_bool ? S_alpha1str * texture(S_alpha1_map, IO_uv2).g : 1.0;
  float a2 = S_alpha2_bool ? S_alpha2str * texture(S_alpha2_map, IO_uv2).g : 1.0;
  float a3 = S_alpha3_bool ? S_alpha3str * texture(S_alpha3_map, IO_uv3).g : 1.0;

  vec3 col = base.rgb * U_color_emissive.rgb * a1 * a2 * a3;
  col = gradeMatrix(U_base_brightness_shift, U_base_contrast_shift, U_base_saturation_shift,
                    U_base_hue_shift) *
        vec4(col, 1.0);

#ifdef STATION_HULL
  // Forcefields glow in a thin band just in front of whatever they intersect, plus a faint pulse.
  if (forcefield == 1) {
    float band = 0.0;
    if (hasSceneDepth == 1) {
      float gap = abs(viewDepth(texture(sceneDepth, gl_FragCoord.xy / depthViewport).r) -
                      viewDepth(gl_FragCoord.z));
      float edge = softRange.x + 0.2 * softRange.y;
      band = softAtt(softRange.x, 0.2 * softRange.y, gap) *
             (1.0 - softAtt(edge, softRange.y + 4.0 * base.r, gap));
    }
    col = saturate(col) * max(0.25 * band, 0.01 * max(0.0, sin(F_time)));
  }
#endif

  col *= IO_Fade * U_useralphascale;

#ifdef STATION_HULL
  if (opaqueUnlit == 1) {
    OUT_RT0 = vec4(clamp(col * colDiffuse.rgb, 0.0, 1000.0), 1.0);
    return;
  }
  // Cross-fading LOD levels sum to one copy.
  col *= colDiffuse.rgb * (lodFadeInvert == 1 ? 1.0 - lodFade : lodFade);
#endif
  OUT_RT0 = vec4(clamp(col, 0.0, 1000.0), saturate(base.a * a2 * a3 * F_alphascale * IO_Fade));
}
)";

}  // namespace x4sb::editor::planet_glsl

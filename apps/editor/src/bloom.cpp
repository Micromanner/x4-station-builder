#include "bloom.hpp"

#include "external/glad.h"
#include "rlgl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace x4sb::editor {
namespace {

// Every constant below is the value X4 bound in the captured frame (sun.rdc
// draw eids 9482-9784, tools/rd_draws.json) — not a tuning knob.

// kinobloom/prefilter.frag.glsl with U_pass=1 (median antiflicker ON),
// U_lens_pass=0 (ghosts OFF), U_threshold=7.0, U_color=(3.5, 7.0, 1/14) =
// (threshold-knee, 2*knee, 0.25/knee), U_bias=0. Runs at FULL resolution.
// Brightness() is the max channel. The 0.6 dimming per below-threshold
// neighbour is X4's own "optional input limiting" — shipped enabled.
constexpr const char* kPrefilterFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform float exposure;
out vec4 finalColor;
const float kThreshold = 7.0;
const vec3 kCurve = vec3(3.5, 7.0, 0.0714285714);
vec3 med(vec3 a, vec3 b, vec3 c) { return a + b + c - min(min(a, b), c) - max(max(a, b), c); }
float br(vec3 c) { return max(c.r, max(c.g, c.b)); }
void main() {
  vec2 ts = 1.0 / vec2(textureSize(texture0, 0));
  vec3 p  = texture(texture0, fragTexCoord).rgb * exposure;
  vec3 xm = texture(texture0, fragTexCoord - vec2(ts.x, 0.0)).rgb * exposure;
  vec3 xp = texture(texture0, fragTexCoord + vec2(ts.x, 0.0)).rgb * exposure;
  vec3 ym = texture(texture0, fragTexCoord - vec2(0.0, ts.y)).rgb * exposure;
  vec3 yp = texture(texture0, fragTexCoord + vec2(0.0, ts.y)).rgb * exposure;
  p = med(p, xm, xp);
  p = med(p, ym, yp);
  if (br(xm) < kThreshold) p *= 0.6;
  if (br(xp) < kThreshold) p *= 0.6;
  if (br(ym) < kThreshold) p *= 0.6;
  if (br(yp) < kThreshold) p *= 0.6;
  if (any(isnan(p)) || any(isinf(p))) { finalColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
  float b = br(p);
  float rq = clamp(b - kCurve.x, 0.0, kCurve.y);
  rq = kCurve.z * rq * rq;
  p *= max(rq, b - kThreshold) / max(b, 1e-5);
  finalColor = vec4(p, 1.0);
}
)";

// kinobloom/downsample.frag.glsl: 4 taps at +/-1 SOURCE texel. The first
// level (full-res prefilter -> half) runs with U_pass=1 = Karis average
// (1/(1+max) weights, kills fireflies); every deeper level is the plain box.
constexpr const char* kDownsampleFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform int karis;
out vec4 finalColor;
float br(vec3 c) { return max(c.r, max(c.g, c.b)); }
void main() {
  vec2 ts = 1.0 / vec2(textureSize(texture0, 0));
  vec3 a = textureLod(texture0, fragTexCoord + vec2(-ts.x, -ts.y), 0.0).rgb;
  vec3 b = textureLod(texture0, fragTexCoord + vec2( ts.x, -ts.y), 0.0).rgb;
  vec3 c = textureLod(texture0, fragTexCoord + vec2(-ts.x,  ts.y), 0.0).rgb;
  vec3 d = textureLod(texture0, fragTexCoord + vec2( ts.x,  ts.y), 0.0).rgb;
  vec3 s;
  if (karis == 1) {
    float wa = 1.0 / (br(a) + 1.0), wb = 1.0 / (br(b) + 1.0);
    float wc = 1.0 / (br(c) + 1.0), wd = 1.0 / (br(d) + 1.0);
    s = (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
  } else {
    s = (a + b + c + d) * 0.25;
  }
  finalColor = vec4(s, 1.0);
}
)";

// kinobloom/upsample.frag.glsl HIGH_QUALITY: 9-tap tent on the coarser level
// (U_scale 0.99 ~ one texel, useralphascale 1) accumulated ONE/ONE onto this
// level's downsample. X4 blends into the mip chain in place; the shader-side
// add is the deterministic equivalent.
constexpr const char* kUpsampleFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;  // this level's downsample
uniform sampler2D lowerMap;  // coarser accumulated result
out vec4 finalColor;
void main() {
  vec2 ts = 1.0 / vec2(textureSize(lowerMap, 0));
  vec3 s = textureLod(lowerMap, fragTexCoord + vec2(-ts.x, -ts.y), 0.0).rgb
     + 2.0 * textureLod(lowerMap, fragTexCoord + vec2(0.0, -ts.y), 0.0).rgb
     +       textureLod(lowerMap, fragTexCoord + vec2(ts.x, -ts.y), 0.0).rgb
     + 2.0 * textureLod(lowerMap, fragTexCoord + vec2(-ts.x, 0.0), 0.0).rgb
     + 4.0 * textureLod(lowerMap, fragTexCoord, 0.0).rgb
     + 2.0 * textureLod(lowerMap, fragTexCoord + vec2(ts.x, 0.0), 0.0).rgb
     +       textureLod(lowerMap, fragTexCoord + vec2(-ts.x, ts.y), 0.0).rgb
     + 2.0 * textureLod(lowerMap, fragTexCoord + vec2(0.0, ts.y), 0.0).rgb
     +       textureLod(lowerMap, fragTexCoord + vec2(ts.x, ts.y), 0.0).rgb;
  finalColor = vec4(texture(texture0, fragTexCoord).rgb + s / 16.0, 1.0);
}
)";

// Final bloom add (X4 draw eid 9746: tent of the finest level, useralphascale
// 0.5, ONE/ONE onto the scene) fused with common_tonemap.glsl: the HDR value
// goes straight into the 32^3 tonemap LUT in log10 space (X4's F_aces_scale/bias
// as captured; table from tonemapLut()), then the colour-grade CLUT in gamma space
// (F_lut_blend 1.0). Output stays LINEAR — X4's swapchain is an sRGB format; kEncodeFs is that.
constexpr const char* kTonemapFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;  // linear HDR scene
uniform sampler2D bloomMap;
uniform sampler3D lut;
uniform sampler3D clut;
uniform float exposure;
out vec4 finalColor;
void main() {
  vec3 c = texture(texture0, fragTexCoord).rgb * exposure;
  vec2 ts = 1.0 / vec2(textureSize(bloomMap, 0));
  vec3 b = textureLod(bloomMap, fragTexCoord + vec2(-ts.x, -ts.y), 0.0).rgb
     + 2.0 * textureLod(bloomMap, fragTexCoord + vec2(0.0, -ts.y), 0.0).rgb
     +       textureLod(bloomMap, fragTexCoord + vec2(ts.x, -ts.y), 0.0).rgb
     + 2.0 * textureLod(bloomMap, fragTexCoord + vec2(-ts.x, 0.0), 0.0).rgb
     + 4.0 * textureLod(bloomMap, fragTexCoord, 0.0).rgb
     + 2.0 * textureLod(bloomMap, fragTexCoord + vec2(ts.x, 0.0), 0.0).rgb
     +       textureLod(bloomMap, fragTexCoord + vec2(-ts.x, ts.y), 0.0).rgb
     + 2.0 * textureLod(bloomMap, fragTexCoord + vec2(0.0, ts.y), 0.0).rgb
     +       textureLod(bloomMap, fragTexCoord + vec2(ts.x, ts.y), 0.0).rgb;
  c += b * (0.5 / 16.0);
  vec3 coord = (log2(c + 1e-14) * 0.30103 * 0.2143179327 + 0.6434794068) * (31.0 / 32.0) + (1.0 / 64.0);
  vec3 t = texture(lut, coord).rgb;
  vec3 g = pow(t + 1e-6, vec3(1.0 / 2.2));
  t = pow(texture(clut, g).rgb + 1e-6, vec3(2.2));
  finalColor = vec4(t, 1.0);
}
)";

// chromatic.frag.glsl as bound (U_scale 4): four spectral taps radially
// scaled about the screen centre, on the tonemapped linear image.
constexpr const char* kChromaFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
out vec4 finalColor;
const vec3 kW0 = vec3(0.667, -0.083, 0.417);
const vec3 kW1 = vec3(0.833, 0.583, 0.583);
const vec3 kW2 = vec3(0.583, 0.833, 0.583);
const vec3 kW3 = vec3(-0.083, 0.667, 0.417);
void main() {
  vec2 vp = vec2(textureSize(texture0, 0));
  float ar = vp.y / vp.x;
  vec2 d = fragTexCoord - vec2(0.5);
  d.y *= ar;
  float r = 2.0 * length(d) * inversesqrt(ar * ar + 1.0) * 4.0 * 0.0010416667;
  if (abs(r) < 0.00052083) { finalColor = vec4(texture(texture0, fragTexCoord).rgb, 1.0); return; }
  vec3 acc = vec3(0.0);
  vec3 w[4] = vec3[4](kW0, kW1, kW2, kW3);
  for (int i = 0; i < 4; ++i) {
    vec2 s = d / (1.0 + r * (float(i) / 3.0 - 0.5));
    s.y /= ar;
    acc += w[i] * textureLod(texture0, s + vec2(0.5), 0.0).rgb;
  }
  finalColor = vec4(acc * 0.5, 1.0);
}
)";

// The sRGB swapchain encode (true piecewise curve, like the hardware format).
constexpr const char* kEncodeFs = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
out vec4 finalColor;
vec3 srgb(vec3 c) {
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
void main() {
  finalColor = vec4(srgb(clamp(texture(texture0, fragTexCoord).rgb, 0.0, 1.0)), 1.0);
}
)";

// Fullscreen pass: draw src through `shader` into dst. The negative source
// height keeps every RT's content orientation-consistent (RT textures sample
// upside-down vs raylib's image convention; consistency matters because the
// upsample/tonemap passes read two RTs at once).
void blit(const ::Texture2D& src, const ::RenderTexture2D& dst, ::Shader shader) {
  BeginTextureMode(dst);
  // Explicit blend: the pass must REPLACE the RT content. Without this the
  // pass inherits whatever mode the scene left active — a future additive
  // leak would accumulate stale pyramid texels across frames (ghosting).
  BeginBlendMode(BLEND_ALPHA);
  BeginShaderMode(shader);
  DrawTexturePro(
      src, ::Rectangle{0.0f, 0.0f, static_cast<float>(src.width), -static_cast<float>(src.height)},
      ::Rectangle{0.0f, 0.0f, static_cast<float>(dst.texture.width),
                  static_cast<float>(dst.texture.height)},
      ::Vector2{0.0f, 0.0f}, 0.0f, WHITE);
  EndShaderMode();
  EndBlendMode();
  EndTextureMode();
}

[[nodiscard]] unsigned int loadLut3d(const void* data, std::size_t size, GLenum internal,
                                     GLenum type, std::size_t bytesPerTexel) {
  constexpr int kDim = 32;
  if (size != static_cast<std::size_t>(kDim * kDim * kDim) * bytesPerTexel) return 0;
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_3D, id);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  // Tables are z-major (x fastest), which is exactly what glTexImage3D consumes.
  // Rows are 96/384 bytes, 4-aligned.
  glTexImage3D(GL_TEXTURE_3D, 0, static_cast<GLint>(internal), kDim, kDim, kDim, 0, GL_RGB, type,
               data);
  glBindTexture(GL_TEXTURE_3D, 0);
  return id;
}

// Our fitted model of X4's tonemap LUT (the game builds its table at runtime). Its
// structure follows the public ACES RRT (aces-dev, Academy license): glow and red
// modifier in AP0, a per-channel tone curve, then an ODT-style luminance gamma on AP1
// Y. Every constant below was fitted to match the game's table; the model is within
// 0.018 of it (worst at inputs above 8) and within 2/255 on nearly all frame pixels.
[[nodiscard]] std::array<double, 3> tonemapModel(const std::array<double, 3>& rgb) {
  constexpr std::array<double, 9> kSrgbToAp0{0.439632982,  0.382988698, 0.17737832,
                                             0.089776443,  0.813439429, 0.0967841283,
                                             0.0175411704, 0.111546553, 0.870912276};
  constexpr std::array<double, 9> kIn{3.31239975,   -0.560560931, -0.495332571,
                                      -0.182594881, 2.66365103,   -0.223320835,
                                      0.0340379822, 0.028976936,  2.21466999};
  constexpr std::array<double, 9> kOut{8.09326519,    -2.52768818,  -0.37157078,
                                       -0.488076362,  5.73109698,   -0.0313678309,
                                       -0.0135966648, -0.369082609, 5.574282};
  constexpr std::array<double, 3> kAp1Luma{0.272228717, 0.674081766, 0.0536895174};
  constexpr double kLumaGamma = 0.808033387;
  constexpr double kGlowGain = 0.0527205723;
  constexpr double kGlowMid = 0.0932023437;
  constexpr double kRedScale = 0.83635492;
  constexpr double kRedPivot = 0.0326581743;
  constexpr double kRedWidth = 140.139914;
  constexpr double kToeOffset = 0.0032335557;
  constexpr double kToePower = 1.1983665;
  constexpr double kToeScale = 0.0767899955;
  constexpr double kShoulderStart = 0.434988916;
  constexpr double kShoulderMax = 0.127773994;
  constexpr double kShoulderSharpness = 1.09733735;

  const auto mul = [](const std::array<double, 9>& m, const std::array<double, 3>& v) {
    return std::array<double, 3>{m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
                                 m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                                 m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
  };
  std::array<double, 3> a = mul(kSrgbToAp0, rgb);

  const double hi = std::max({a[0], a[1], a[2]});
  const double lo = std::min({a[0], a[1], a[2]});
  const double sat = (std::max(hi, 1e-10) - std::max(lo, 1e-10)) / std::max(hi, 1e-2);
  const double chroma =
      std::sqrt(std::max(a[2] * (a[2] - a[1]) + a[1] * (a[1] - a[0]) + a[0] * (a[0] - a[2]), 0.0));
  const double yc = (a[0] + a[1] + a[2] + 1.75 * chroma) / 3.0;
  const double sx = (sat - 0.4) / 0.2;
  const double st = std::max(1.0 - std::abs(sx / 2.0), 0.0);
  const double sign = sx > 0.0 ? 1.0 : (sx < 0.0 ? -1.0 : 0.0);
  const double glowGain = kGlowGain * (1.0 + sign * (1.0 - st * st)) / 2.0;
  double glow = 0.0;
  if (yc <= 2.0 / 3.0 * kGlowMid) {
    glow = glowGain;
  } else if (yc < 2.0 * kGlowMid) {
    glow = glowGain * (kGlowMid / yc - 0.5);
  }
  for (double& c : a) c *= 1.0 + glow;

  constexpr double kDegrees = 57.29577951308232;
  const double hue =
      std::atan2(std::sqrt(3.0) * (a[1] - a[2]), 2.0 * a[0] - a[1] - a[2]) * kDegrees;
  if (hue > -kRedWidth / 2.0 && hue < kRedWidth / 2.0) {
    const double knot = (hue + kRedWidth / 2.0) * 4.0 / kRedWidth;
    const double t = knot - std::floor(knot);
    const double t2 = t * t;
    const double t3 = t2 * t;
    double weight = 0.0;
    switch (static_cast<int>(std::floor(knot))) {
      case 0:
        weight = t3 / 6.0;
        break;
      case 1:
        weight = (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0;
        break;
      case 2:
        weight = (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0;
        break;
      case 3:
        weight = (-t3 + 3.0 * t2 - 3.0 * t + 1.0) / 6.0;
        break;
      default:
        break;
    }
    a[0] += 1.5 * weight * sat * (kRedPivot - a[0]) * (1.0 - kRedScale);
  }

  // Power toe joined C1 at kShoulderStart to a rational shoulder rising to kShoulderMax.
  const double startY = kToeScale * std::pow(kShoulderStart - kToeOffset, kToePower);
  const double startSlope =
      kToeScale * kToePower * std::pow(kShoulderStart - kToeOffset, kToePower - 1.0);
  const double range = kShoulderMax - startY;
  std::array<double, 3> c = mul(kIn, a);
  for (double& v : c) {
    if (v < kShoulderStart) {
      v = kToeScale * std::pow(std::max(v - kToeOffset, 0.0), kToePower);
    } else {
      const double u = startSlope * (v - kShoulderStart) / range;
      v = startY +
          range * u / std::pow(1.0 + std::pow(u, kShoulderSharpness), 1.0 / kShoulderSharpness);
    }
  }

  const double luma = std::max(kAp1Luma[0] * c[0] + kAp1Luma[1] * c[1] + kAp1Luma[2] * c[2], 1e-6);
  const double lumaScale = std::pow(luma, kLumaGamma - 1.0);
  for (double& v : c) v *= lumaScale;
  std::array<double, 3> out = mul(kOut, c);
  for (double& v : out) v = std::clamp(v, 0.0, 1.0);
  return out;
}

// The tonemap table in the shader's layout: texel i holds the model at the linear
// input its log10 coordinate addresses (X4's F_aces_scale/bias).
[[nodiscard]] std::vector<float> tonemapLut() {
  constexpr int kDim = 32;
  std::array<double, kDim> input{};
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = std::pow(10.0, (static_cast<double>(i) / 31.0 - 0.6434794068) / 0.2143179327);
  }
  std::vector<float> data;
  data.reserve(static_cast<std::size_t>(kDim * kDim * kDim) * 3);
  for (const double b : input) {
    for (const double g : input) {
      for (const double r : input) {
        for (const double v : tonemapModel({r, g, b})) data.push_back(static_cast<float>(v));
      }
    }
  }
  return data;
}

// X4's captured grade CLUT is a neutral ramp, 8*i - 1 per channel (0 at i = 0),
// so it is generated rather than embedded. Same z-major RGB8 layout as the blob.
[[nodiscard]] std::vector<unsigned char> gradeClut() {
  constexpr int kDim = 32;
  std::vector<unsigned char> data;
  data.reserve(static_cast<std::size_t>(kDim * kDim * kDim) * 3);
  const auto ramp = [](int i) { return static_cast<unsigned char>(i == 0 ? 0 : 8 * i - 1); };
  for (int b = 0; b < kDim; ++b) {
    for (int g = 0; g < kDim; ++g) {
      for (int r = 0; r < kDim; ++r) {
        data.push_back(ramp(r));
        data.push_back(ramp(g));
        data.push_back(ramp(b));
      }
    }
  }
  return data;
}

}  // namespace

// RGBA16F color target; depth renderbuffer only when the 3D scene renders into
// it (the pyramid levels are 2D-only). Mirrors raylib's LoadRenderTexture
// bookkeeping so BeginTextureMode/EndTextureMode work unmodified.
::RenderTexture2D loadHdrTarget(int width, int height, bool withDepth) {
  ::RenderTexture2D target{};
  target.id = rlLoadFramebuffer();
  if (target.id == 0) return target;
  rlEnableFramebuffer(target.id);

  target.texture.id =
      rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, 1);
  target.texture.width = width;
  target.texture.height = height;
  target.texture.format = PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
  target.texture.mipmaps = 1;
  rlFramebufferAttach(target.id, target.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0,
                      RL_ATTACHMENT_TEXTURE2D, 0);

  if (withDepth) {
    target.depth.id = rlLoadTextureDepth(width, height, true);
    target.depth.width = width;
    target.depth.height = height;
    target.depth.format = 19;  // raylib's DEPTH_COMPONENT_24BIT bookkeeping value
    target.depth.mipmaps = 1;
    rlFramebufferAttach(target.id, target.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER,
                        0);
  }

  if (!rlFramebufferComplete(target.id)) {
    rlUnloadFramebuffer(target.id);
    if (target.texture.id != 0) rlUnloadTexture(target.texture.id);
    return ::RenderTexture2D{};
  }
  rlDisableFramebuffer();
  // The tent/box filters rely on bilinear taps (X4 samples rt_clamp_linear).
  SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(target.texture, TEXTURE_WRAP_CLAMP);
  return target;
}

void unloadHdrTarget(::RenderTexture2D& target) {
  if (target.id != 0) UnloadRenderTexture(target);  // frees fbo + color + depth
  target = ::RenderTexture2D{};
}

FramePipeline::~FramePipeline() {
  releaseTargets();
  if (shadersOk_) {
    UnloadShader(prefilter_);
    UnloadShader(downsample_);
    UnloadShader(upsample_);
    UnloadShader(tonemap_);
    UnloadShader(chroma_);
    UnloadShader(encode_);
  }
  if (lut3d_ != 0) glDeleteTextures(1, &lut3d_);
  if (clut3d_ != 0) glDeleteTextures(1, &clut3d_);
}

bool FramePipeline::loadMsaaScene(int width, int height) {
  // Raw GL: raylib has no multisampled render textures. Colour is a texture, not a
  // renderbuffer, so a compute pass can store per sample into it (visibility-buffer
  // spec 2026-10-14 §3.4).
  glGenTextures(1, &msaaColor_);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msaaColor_);
  glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, kMsaaSamples, GL_RGBA16F, width, height,
                            GL_TRUE);
  // Depth too, so the Hi-Z build can read every sample (hiz spec 2026-10-15 §2.3).
  glGenTextures(1, &msaaDepth_);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msaaDepth_);
  glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, kMsaaSamples, GL_DEPTH_COMPONENT24, width,
                            height, GL_TRUE);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE,
                         msaaColor_, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, msaaDepth_,
                         0);
  const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  scene_.id = fbo;
  scene_.texture.width = width;
  scene_.texture.height = height;
  return ok;
}

void FramePipeline::unloadMsaaScene() {
  if (scene_.id != 0) glDeleteFramebuffers(1, &scene_.id);
  if (msaaColor_ != 0) glDeleteTextures(1, &msaaColor_);
  if (msaaDepth_ != 0) glDeleteTextures(1, &msaaDepth_);
  scene_ = ::RenderTexture2D{};
  msaaColor_ = 0;
  msaaDepth_ = 0;
}

void FramePipeline::releaseTargets() {
  unloadMsaaScene();
  unloadHdrTarget(resolved_);
  unloadHdrTarget(prefiltered_);
  for (auto& t : down_) unloadHdrTarget(t);
  for (auto& t : up_) unloadHdrTarget(t);
  unloadHdrTarget(ldr_);
  unloadHdrTarget(ldr2_);
  levels_ = 0;
  width_ = 0;
  height_ = 0;
}

void FramePipeline::ensureLuts() {
  if (lut3d_ != 0) return;
  const std::vector<float> lut = tonemapLut();
  lut3d_ = loadLut3d(lut.data(), lut.size() * sizeof(float), GL_RGB16F, GL_FLOAT, 12);
  const std::vector<unsigned char> clut = gradeClut();
  clut3d_ = loadLut3d(clut.data(), clut.size(), GL_RGB8, GL_UNSIGNED_BYTE, 3);
}

void FramePipeline::ensureShaders() {
  if (shadersLoaded_) return;
  shadersLoaded_ = true;
  ensureLuts();
  prefilter_ = LoadShaderFromMemory(nullptr, kPrefilterFs);  // default fullscreen VS
  downsample_ = LoadShaderFromMemory(nullptr, kDownsampleFs);
  upsample_ = LoadShaderFromMemory(nullptr, kUpsampleFs);
  tonemap_ = LoadShaderFromMemory(nullptr, kTonemapFs);
  chroma_ = LoadShaderFromMemory(nullptr, kChromaFs);
  encode_ = LoadShaderFromMemory(nullptr, kEncodeFs);
  shadersOk_ = IsShaderValid(prefilter_) && IsShaderValid(downsample_) &&
               IsShaderValid(upsample_) && IsShaderValid(tonemap_) && IsShaderValid(chroma_) &&
               IsShaderValid(encode_) && lut3d_ != 0 && clut3d_ != 0;
  if (!shadersOk_) {
    // Partial-failure path: some may have loaded ok even though the group
    // didn't — unload those so we don't leak GL shader program objects.
    auto unloadIfLoaded = [](::Shader& s) {
      if (s.id != 0 && s.id != rlGetShaderIdDefault()) UnloadShader(s);
      s = ::Shader{};
    };
    unloadIfLoaded(prefilter_);
    unloadIfLoaded(downsample_);
    unloadIfLoaded(upsample_);
    unloadIfLoaded(tonemap_);
    unloadIfLoaded(chroma_);
    unloadIfLoaded(encode_);
    return;
  }
  locDownsampleKaris_ = GetShaderLocation(downsample_, "karis");
  locUpsampleLower_ = GetShaderLocation(upsample_, "lowerMap");
  locTonemapBloom_ = GetShaderLocation(tonemap_, "bloomMap");
  locTonemapLut_ = GetShaderLocation(tonemap_, "lut");
  locTonemapClut_ = GetShaderLocation(tonemap_, "clut");
  locTonemapExposure_ = GetShaderLocation(tonemap_, "exposure");
}

bool FramePipeline::ensureTargets(int width, int height) {
  if (width == width_ && height == height_) return targetsReady();
  releaseTargets();
  width_ = width;
  height_ = height;
  if (!loadMsaaScene(width, height)) return false;
  resolved_ = loadHdrTarget(width, height, /*withDepth=*/false);
  prefiltered_ = loadHdrTarget(width, height, /*withDepth=*/false);
  ldr_ = loadHdrTarget(width, height, /*withDepth=*/false);
  ldr2_ = loadHdrTarget(width, height, /*withDepth=*/false);
  if (resolved_.id == 0 || prefiltered_.id == 0 || ldr_.id == 0 || ldr2_.id == 0) return false;

  levels_ = 0;
  int lw = width / 2;
  int lh = height / 2;
  while (levels_ < kMaxLevels && lw >= 4 && lh >= 4) {
    down_[static_cast<std::size_t>(levels_)] = loadHdrTarget(lw, lh, /*withDepth=*/false);
    if (down_[static_cast<std::size_t>(levels_)].id == 0) return false;
    if (levels_ + 1 < kMaxLevels) {  // up_[last] is never written
      up_[static_cast<std::size_t>(levels_)] = loadHdrTarget(lw, lh, /*withDepth=*/false);
      if (up_[static_cast<std::size_t>(levels_)].id == 0) return false;
    }
    ++levels_;
    lw /= 2;
    lh /= 2;
  }
  return targetsReady();
}

// A failed or partial build must keep failing at the same size, not pass the early-out.
bool FramePipeline::targetsReady() const {
  return resolved_.id != 0 && prefiltered_.id != 0 && ldr_.id != 0 && ldr2_.id != 0 && levels_ > 0;
}

void FramePipeline::begin() {
  ensureShaders();
  active_ = shadersOk_ && ensureTargets(GetScreenWidth(), GetScreenHeight());
  if (!active_) {
    if (!warned_) {
      TraceLog(LOG_WARNING,
               "FramePipeline: HDR target unavailable - rendering linear to backbuffer");
      warned_ = true;
    }
    return;
  }
  BeginTextureMode(scene_);
}

void FramePipeline::end(const std::function<void()>& ldrOverlay) {
  if (!active_) return;
  EndTextureMode();

  // Box resolve: every sample weighs the same, so HDR energy (bloom input, sun
  // and star brightness) is exactly what a single-sample frame would carry.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_.id);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolved_.id);
  glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Kinobloom: full-res prefilter -> Karis half -> box chain -> tent accumulate.
  SetShaderValue(prefilter_, GetShaderLocation(prefilter_, "exposure"), &sceneExposure_,
                 SHADER_UNIFORM_FLOAT);
  blit(resolved_.texture, prefiltered_, prefilter_);
  for (int i = 0; i < levels_; ++i) {
    const int karis = i == 0 ? 1 : 0;
    SetShaderValue(downsample_, locDownsampleKaris_, &karis, SHADER_UNIFORM_INT);
    blit(i == 0 ? prefiltered_.texture : down_[static_cast<std::size_t>(i - 1)].texture,
         down_[static_cast<std::size_t>(i)], downsample_);
  }
  const ::Texture2D* lower = &down_[static_cast<std::size_t>(levels_ - 1)].texture;
  for (int i = levels_ - 2; i >= 0; --i) {
    const auto ui = static_cast<std::size_t>(i);
    BeginTextureMode(up_[ui]);
    BeginBlendMode(BLEND_ALPHA);  // replace, same rationale as blit()
    BeginShaderMode(upsample_);
    const int slot = 1;  // slot-1 bind persists through the batch flush (mesh_cache pattern)
    rlActiveTextureSlot(1);
    rlEnableTexture(lower->id);
    SetShaderValue(upsample_, locUpsampleLower_, &slot, SHADER_UNIFORM_INT);
    DrawTexturePro(down_[ui].texture,
                   ::Rectangle{0.0f, 0.0f, static_cast<float>(down_[ui].texture.width),
                               -static_cast<float>(down_[ui].texture.height)},
                   ::Rectangle{0.0f, 0.0f, static_cast<float>(up_[ui].texture.width),
                               static_cast<float>(up_[ui].texture.height)},
                   ::Vector2{0.0f, 0.0f}, 0.0f, WHITE);
    EndShaderMode();
    EndBlendMode();
    EndTextureMode();
    rlActiveTextureSlot(0);
    lower = &up_[ui].texture;
  }
  const ::Texture2D& bloomTex = *lower;

  // Tonemap (bloom add fused) into the linear LDR target. The 3D LUTs sit on
  // slots raylib never touches (2/3); mesh_cache owns slot 9 for the env map.
  BeginTextureMode(ldr_);
  BeginBlendMode(BLEND_ALPHA);
  BeginShaderMode(tonemap_);
  const int bloomSlot = 1;
  const int lutSlot = 2;
  const int clutSlot = 3;
  rlActiveTextureSlot(1);
  rlEnableTexture(bloomTex.id);
  glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(lutSlot));
  glBindTexture(GL_TEXTURE_3D, lut3d_);
  glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(clutSlot));
  glBindTexture(GL_TEXTURE_3D, clut3d_);
  rlActiveTextureSlot(0);
  SetShaderValue(tonemap_, locTonemapBloom_, &bloomSlot, SHADER_UNIFORM_INT);
  SetShaderValue(tonemap_, locTonemapLut_, &lutSlot, SHADER_UNIFORM_INT);
  SetShaderValue(tonemap_, locTonemapClut_, &clutSlot, SHADER_UNIFORM_INT);
  SetShaderValue(tonemap_, locTonemapExposure_, &sceneExposure_, SHADER_UNIFORM_FLOAT);
  DrawTexturePro(resolved_.texture,
                 ::Rectangle{0.0f, 0.0f, static_cast<float>(resolved_.texture.width),
                             -static_cast<float>(resolved_.texture.height)},
                 ::Rectangle{0.0f, 0.0f, static_cast<float>(ldr_.texture.width),
                             static_cast<float>(ldr_.texture.height)},
                 ::Vector2{0.0f, 0.0f}, 0.0f, WHITE);
  EndShaderMode();
  EndBlendMode();
  EndTextureMode();

  blit(ldr_.texture, ldr2_, chroma_);

  if (ldrOverlay) {
    BeginTextureMode(ldr2_);
    ldrOverlay();
    EndTextureMode();
  }

  if (!dumpPrefix_.empty()) {
    const auto dump = [this](const ::RenderTexture2D& rt, const char* name) {
      ::Image img = LoadImageFromTexture(rt.texture);
      // The screen-size HDR frame also goes out unconverted (RGBA16F, bottom-up,
      // no header) so tools can measure linear energy the PNG clamp destroys.
      if (rt.id == resolved_.id) ExportImage(img, (dumpPrefix_ + "_" + name + ".raw").c_str());
      ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
      ImageFlipVertical(&img);
      ExportImage(img, (dumpPrefix_ + "_" + name + ".png").c_str());
      UnloadImage(img);
    };
    dump(resolved_, "scene");
    dump(ldr_, "ldr");
    dump(ldr2_, "ldr2");
    dumpPrefix_.clear();
  }

  BeginShaderMode(encode_);
  BeginBlendMode(BLEND_ALPHA);
  DrawTexturePro(ldr2_.texture,
                 ::Rectangle{0.0f, 0.0f, static_cast<float>(ldr2_.texture.width),
                             -static_cast<float>(ldr2_.texture.height)},
                 ::Rectangle{0.0f, 0.0f, static_cast<float>(GetScreenWidth()),
                             static_cast<float>(GetScreenHeight())},
                 ::Vector2{0.0f, 0.0f}, 0.0f, WHITE);
  EndBlendMode();
  EndShaderMode();
}

}  // namespace x4sb::editor

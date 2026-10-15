#pragma once
// X4 texture-scroll animation (<uv_animations>): the geometry stays put and a UV
// set slides, turns or tiles, which is all the anim_* motion X4 ships as data
// (arg-cross arrows, blinking position lights, planet bands, sun coronas). Spec:
// docs/superpowers/specs/2026-06-16-uv-scroll-animation-design.md.
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace x4sb {

inline constexpr double kAnimFps = 30.0;  // X4 frame counts are exact at 30 fps
inline constexpr int kUvSets = 4;         // X4's M_texturematrix0..3
// Frames and offsets beyond this are rejected on read: finite but huge values
// would overflow the curve maths to NaN UVs.
inline constexpr double kMaxUvAnimationMagnitude = 1e9;

// How the curve leaves a key (the key's out-tangent).
enum class UvInterp : std::uint8_t { Linear, Smooth, Step };

struct UvKey {
  double frame{};
  double value{};
  UvInterp interp{UvInterp::Linear};
};

// Curves of one UV set; any may be empty (offset 0, rotation 0, tile 1). Keys
// sorted by frame.
struct UvChannel {
  int uvSet{0};  // XMF TEXCOORD index: X4 mapchannel - 1
  std::vector<UvKey> u;
  std::vector<UvKey> v;
  std::vector<UvKey> rotation;  // degrees
  std::vector<UvKey> uTile;
  std::vector<UvKey> vTile;
  bool aroundCenter{};  // rotaroundcenter: turn and tile about UV (0.5, 0.5), not the origin
};

struct UvAnimation {
  double loopStart{};  // frames; the connection's <animation name="loop">
  double loopEnd{};
  std::vector<UvChannel> channels;  // empty: not animated
};

struct UvOffset {
  float u{};
  float v{};
};

// Per UV set uv' = linear * uv + offset: X4's mat3x2(M_texturematrixN). Split
// into two arrays so each uploads as one shader uniform array.
struct UvTransforms {
  // Column-major 2x2 (a GLSL mat2(vec4)).
  std::array<std::array<float, 4>, kUvSets> linear{
      {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}}};
  std::array<UvOffset, kUvSets> offset{};
};

// Transforms per UV set at `seconds` on a looping clock; identity when not animated.
[[nodiscard]] UvTransforms evalUvAnimation(const UvAnimation& anim, double seconds);

// Cache round-trip (catalog.json, background manifest). Reading validates, so a
// malformed record yields fewer channels, never out-of-range UV sets.
[[nodiscard]] nlohmann::json uvAnimationToJson(const UvAnimation& anim);
[[nodiscard]] UvAnimation uvAnimationFromJson(const nlohmann::json& j);

}  // namespace x4sb

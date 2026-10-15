#pragma once
// X4 lens-effect library parsing (sun rendering, approach-B spec
// docs/superpowers/specs/2026-07-19-sun-lens-effect-design.md). The in-game
// sun has no mesh: it is a stack of flare sprite elements from
// libraries/lens_effects.xml, referenced by the cluster omni's `lenseffect`.
// Render-free; cat-sourced XML is hostile input.
#include "x4sb/assetpipe/xmf.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace x4sb::assetpipe {

struct LensElement {
  std::string part;      // sprite part in assets/fx/lensflares/lensflares.xml
  float position{1.0F};  // 1.0 = at the sun; the rest sit at position x sun NDC (ghosts)
  float minSize{0.0F};
  float maxSize{0.0F};
  float rotation{0.0F};  // authored roll steps; rendered as a fixed roll
  bool glow{false};
  bool shineBehind{false};
};

struct LensEffect {
  float scale{100.0F};  // <parameters scale>: divisor for screen-relative sizes
  std::vector<LensElement> elements;
};

using LensEffectMap = std::map<std::string, LensEffect>;

// Parse a full <lenseffects> document. Inactive (active="0"), unnamed-part and
// non-finite/negative-size elements are dropped; malformed docs yield {}.
[[nodiscard]] LensEffectMap parseLensEffects(const std::string& xml);

// Apply a DLC <diff> doc: <lenseffect> children of add[@sel="/lenseffects"]
// are inserted, overwriting same-name entries (DLC wins). Non-diff or
// malformed docs are ignored.
void mergeLensEffectsDiff(LensEffectMap& effects, const std::string& diffXml);

// A resolved lens-effect sprite, ready for the background manifest. `size` is
// radians when !screenMode (angular = 2*atan(halfWorldSize/sunDistance)),
// else authored size / effect scale — the captured game (sun.rdc) draws the
// screen elements at maxsize as quads of world height `size` at z=100 through
// the main projection, i.e. screen-height fraction = size / tan(fovy/2).
// A flare part's lod0 mesh, flattened to its authored plane: X4 draws every
// lens element as this geometry (unit-sized, x/y in -0.5..0.5) scaled by the
// element size, so the sprite's shape, atlas sub-rect (incl. mirrored
// quadrants and wrapped UVs) and per-vertex tint/falloff all live here.
struct FlareMesh {
  std::vector<float> xy;             // 2 per vertex
  std::vector<float> uv;             // 2 per vertex
  std::vector<std::uint8_t> rgb;     // 3 per vertex, sRGB bytes (white when unpainted)
  std::vector<std::uint32_t> index;  // triangle list
};

struct SunSprite {
  std::string texFile;  // manifest-relative, "lensflares/<atlas>.dds"
  FlareMesh mesh;
  bool screenMode{false};
  float size{0.0F};
  float rotation{0.0F};
  bool glow{false};
  bool shineBehind{false};
  float intensity{1.0F};  // material diffuseStr: X4's authored per-element brightness
  float position{1.0F};   // element position; ghosts (!=1) are drawn at position x sun NDC
  // Material color_emissive (sRGB bytes): p1_simple_unlit multiplies it in.
  std::array<std::uint8_t, 3> emissive{255, 255, 255};
  bool screenBlend{false};  // material blendmode SCREEN (xu_simple_hdr_out fx)
  // p1_complex_unlit's base colour grade (brightness, contrast, saturation,
  // hue degrees); identity for the simple_unlit family, which has none.
  std::array<float, 4> grade{1.0F, 1.0F, 1.0F, 0.0F};
};

// Flatten a part's lod0 mesh (hostile input: only in-range triangles and
// finite vertices survive; colors default to white). Empty when unusable.
[[nodiscard]] FlareMesh flareMeshFromXmf(const XmfMesh& mesh);

// Classify + convert one element. Sizes >= kWorldSizeCutoff are world
// metres at the sun's distance (nullopt when distance is unknown: the angular
// size is underivable); smaller ones are screen-relative via `effectScale`.
// `diffuseStr` is the flare material's brightness multiplier (non-finite or
// non-positive values fall back to 1.0).
[[nodiscard]] std::optional<SunSprite> makeSunSprite(const LensElement& el, float effectScale,
                                                     double sunDistance, const std::string& texFile,
                                                     float diffuseStr = 1.0F);

}  // namespace x4sb::assetpipe

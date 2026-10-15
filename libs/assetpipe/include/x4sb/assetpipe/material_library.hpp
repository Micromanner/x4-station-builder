#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace x4sb {

class Archive;

// Every authored <property>, keyed by lower-cased name: X4 binds material
// properties to shader parameters case-insensitively ("AnisoX" feeds
// `anisox`). Shaders with dozens of inputs (the planet family) read these
// instead of growing MaterialDef's typed fields.
// bugprone-exception-escape here and on MaterialDef is an MSVC-STL-only
// finding (its map move allocates); the libstdc++ build moves maps noexcept.
struct MaterialProperties {                            // NOLINT(bugprone-exception-escape)
  std::map<std::string, float> floats;                 // Float, Int and Integer
  std::map<std::string, std::array<float, 4>> colors;  // Color r,g,b,a as authored, 0..255
  std::map<std::string, std::string> bitmaps;          // archive address as authored
};

struct MaterialDef {  // NOLINT(bugprone-exception-escape)
  std::string diffuseMap;
  // X4 `color_emissive` (Color prop), the unlit-fx tint: the lens-flare shader
  // multiplies diffuse x diffuseStr x this (p1_simple_unlit, captured 2026-09-19).
  std::array<std::uint8_t, 3> colorEmissive{255, 255, 255};
  std::string normalMap;
  std::string specularMap;
  std::string metalMap;
  std::string smoothMap;
  std::string emissiveMap;
  // X4 detail layer (spec 2026-07-10-detail-maps-design.md): the diffuse detail
  // map is CHANNEL-PACKED (R multiplies diffuse, G/B overlay smooth/metal); the
  // normal detail map adds signed XY onto the base normal. Both tile UV0.
  std::string diffuseDetailMap;
  std::string normalDetailMap;
  std::string blendMode;    // raw X4 blendmode attribute; "" when absent (opaque)
  std::string shader;       // raw X4 shader attribute, e.g. "p1_planet_earthlike"
  float diffuseStr = 1.0F;  // HDR diffuse multiplier (unlit fx materials)
  // XML *_detail_tiling. X4's shipped shaders scale UV0 by it on BOTH axes: the
  // library's *_tiling_v values are never read (X4_Station_FRONT/X4_SHOWROOM decompiles).
  float diffuseDetailTiling = 1.0F;
  float diffuseDetailStr = 1.0F;
  float normalDetailTiling = 1.0F;
  float normalDetailStr = 1.0F;
  // X4 scalar props (complex_surface.frag.glsl: Val = scalar, then *= map.r
  // when the map exists). optional because PRESENCE is semantic: a present
  // prop with no map becomes a constant channel; an absent one keeps the
  // legacy no-map default. Spec 2026-07-11-lighting-rest-design.md.
  std::optional<float> smoothness;
  std::optional<float> metallness;
  // X4 glow chain (complex_surface_pom.frag.glsl ~203-232): the glow map's RGB
  // is boosted by alpha (1-10x), hue-rotated by (0.50-hue)*360deg about the
  // grey axis with `saturation` applied first, then scaled by glowStr. Defaults
  // are the engine's identity values (shadergl/ogl/p2_complex_surface.xml).
  float glowHue = 0.5F;
  float glowSaturation = 1.0F;
  float glowStr = 1.0F;
  MaterialProperties properties;
};

class MaterialLibrary {
 public:
  MaterialLibrary() = default;
  explicit MaterialLibrary(const std::string& xmlBytes);

  // Merge another material_library.xml (each DLC ships its own); on a duplicate
  // collection.material key the later add wins, matching X4's patch order.
  void addXml(const std::string& xmlBytes);

  [[nodiscard]] std::optional<MaterialDef> getMaterial(const std::string& ref) const;
  [[nodiscard]] std::size_t size() const { return materials_.size(); }
  [[nodiscard]] std::string toCacheJson() const;
  [[nodiscard]] static std::optional<MaterialLibrary> fromCacheJson(const std::string& bytes);

 private:
  std::unordered_map<std::string, MaterialDef> materials_;
};

// Merge every extension's material libraries (base first, so DLC patches win on
// duplicate refs). X4 7.x ships a second base file, material_library_1.xml,
// alongside the original — try both per prefix; DLCs currently ship only the
// unsuffixed one, so absence is normal.
[[nodiscard]] MaterialLibrary mergedMaterialLibrary(const Archive& archive);

}  // namespace x4sb

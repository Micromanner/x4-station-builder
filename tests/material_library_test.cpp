#include "x4sb/assetpipe/material_library.hpp"

#include <doctest/doctest.h>

#include <array>
#include <string>

using namespace x4sb;

TEST_CASE("MaterialLibrary parses the blendmode attribute and diffuseStr property") {
  // Attribute order and property style mirror the real game file (type-then-name).
  const char* xml = R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="fx">
    <material name="cone" shader="p1_complex_unlit" blendmode="ADDITIVE" preview="none">
      <properties>
        <property type="BitMap" name="diffuse_map" value="t\b" />
        <property type="Float" name="diffuseStr" value="5.0" />
        <property type="Float" name="normalStr" value="9.0" />
      </properties>
    </material>
    <material name="plain">
      <properties>
        <property type="BitMap" name="diffuse_map" value="t\d" />
      </properties>
    </material>
    <material name="blendonly" blendmode="ALPHA1"><properties/></material>
  </collection>
</materiallibrary>)";
  const MaterialLibrary lib{std::string(xml)};

  const auto cone = lib.getMaterial("fx.cone");
  REQUIRE(cone.has_value());
  CHECK(cone->blendMode == "ADDITIVE");
  // A stray non-diffuseStr Float property (normalStr) must not affect this field.
  CHECK(cone->diffuseStr == doctest::Approx(5.0F));

  // Absent attribute/property -> defaults ("" / 1.0), the opaque baseline.
  const auto plain = lib.getMaterial("fx.plain");
  REQUIRE(plain.has_value());
  CHECK(plain->blendMode.empty());
  CHECK(plain->diffuseStr == doctest::Approx(1.0F));

  // blendmode and diffuseStr are independent: a material can set one without the other.
  const auto blendOnly = lib.getMaterial("fx.blendonly");
  REQUIRE(blendOnly.has_value());
  CHECK(blendOnly->blendMode == "ALPHA1");
  CHECK(blendOnly->diffuseStr == doctest::Approx(1.0F));
}

#include "x4sb/archive/archive.hpp"

#include <filesystem>
#include <fstream>

TEST_CASE("mergedMaterialLibrary merges material_library.xml AND material_library_1.xml") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "x4sb_matlib_merge_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  std::string catStr;
  std::string datStr;
  const auto addFile = [&](const std::string& path, const std::string& content) {
    catStr += path + " " + std::to_string(content.size()) + " 1 aa\n";
    datStr += content;
  };
  // X4 7.x splits the base library across two files; both must merge.
  addFile("libraries/material_library.xml", R"(<materiallibrary>
    <collection name="base"><material name="a"><properties>
      <property type="BitMap" name="diffuse_map" value="t\a"/>
    </properties></material></collection></materiallibrary>)");
  addFile("libraries/material_library_1.xml", R"(<materiallibrary>
    <collection name="p2gen"><material name="b" blendmode="ADDITIVE"><properties>
      <property type="BitMap" name="diffuse_map" value="t\b"/>
    </properties></material></collection></materiallibrary>)");
  std::ofstream(dir / "01.cat", std::ios::binary) << catStr;
  std::ofstream(dir / "01.dat", std::ios::binary) << datStr;

  Archive ar;
  REQUIRE(ar.addCatalog((dir / "01.cat").string()));
  const MaterialLibrary lib = mergedMaterialLibrary(ar);
  CHECK(lib.size() == 2);
  CHECK(lib.getMaterial("base.a").has_value());
  const auto b = lib.getMaterial("p2gen.b");
  REQUIRE(b.has_value());
  CHECK(b->blendMode == "ADDITIVE");

  fs::remove_all(dir, ec);
}

TEST_CASE("MaterialLibrary parses smooth_map (the real roughness source)") {
  // X4 splits metalness (metal_map) and smoothness (smooth_map) into two
  // greyscale masks; the editor shader samples both.
  const char* xml = R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="p1">
    <material name="hull">
      <properties>
        <property type="BitMap" name="metal_map" value="t\hull_metal" />
        <property type="BitMap" name="smooth_map" value="t\hull_smooth" />
      </properties>
    </material>
    <material name="glass">
      <properties>
        <property type="BitMap" name="smooth_map" value="t\glass_smooth" />
      </properties>
    </material>
  </collection>
</materiallibrary>)";
  const MaterialLibrary lib{std::string(xml)};

  const auto hull = lib.getMaterial("p1.hull");
  REQUIRE(hull.has_value());
  CHECK(hull->metalMap == "t\\hull_metal");
  CHECK(hull->smoothMap == "t\\hull_smooth");

  // 282 real materials are smooth-only (glass/characters) — metalMap stays empty.
  const auto glass = lib.getMaterial("p1.glass");
  REQUIRE(glass.has_value());
  CHECK(glass->metalMap.empty());
  CHECK(glass->smoothMap == "t\\glass_smooth");
}

TEST_CASE("MaterialLibrary parses the detail-map properties") {
  // Casings mirror the real library: *_detailStr has a capital S; the tiling
  // names are all-lower; 11 real materials use the normal_detailmap alias.
  const char* xml = R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="p1">
    <material name="hull">
      <properties>
        <property type="BitMap" name="diffuse_detail_map" value="t\gen_detail_diff" />
        <property type="Float" name="diffuse_detail_tiling" value="10.0" />
        <property type="Float" name="diffuse_detail_tiling_v" value="5.0" />
        <property type="Float" name="diffuse_detailStr" value="1.5" />
        <property type="BitMap" name="normal_detail_map" value="t\gen_detail_normal" />
        <property type="Float" name="normal_detail_tiling" value="20.0" />
        <property type="Float" name="normal_detail_tiling_v" value="10.0" />
        <property type="Float" name="normal_detailStr" value="0.05" />
      </properties>
    </material>
    <material name="alias">
      <properties>
        <property type="BitMap" name="normal_detailmap" value="t\alias_normal" />
      </properties>
    </material>
    <material name="plain">
      <properties>
        <property type="BitMap" name="diffuse_map" value="t\d" />
      </properties>
    </material>
  </collection>
</materiallibrary>)";
  const MaterialLibrary lib{std::string(xml)};

  const auto hull = lib.getMaterial("p1.hull");
  REQUIRE(hull.has_value());
  CHECK(hull->diffuseDetailMap == "t\\gen_detail_diff");
  CHECK(hull->diffuseDetailTiling == doctest::Approx(10.0F));
  CHECK(hull->diffuseDetailStr == doctest::Approx(1.5F));
  CHECK(hull->normalDetailMap == "t\\gen_detail_normal");
  CHECK(hull->normalDetailTiling == doctest::Approx(20.0F));
  CHECK(hull->normalDetailStr == doctest::Approx(0.05F));

  // The no-underscore alias the library actually ships in 11 materials.
  const auto alias = lib.getMaterial("p1.alias");
  REQUIRE(alias.has_value());
  CHECK(alias->normalDetailMap == "t\\alias_normal");

  // No detail properties -> empty maps, neutral defaults.
  const auto plain = lib.getMaterial("p1.plain");
  REQUIRE(plain.has_value());
  CHECK(plain->diffuseDetailMap.empty());
  CHECK(plain->normalDetailMap.empty());
  CHECK(plain->diffuseDetailTiling == doctest::Approx(1.0F));
  CHECK(plain->normalDetailTiling == doctest::Approx(1.0F));
}

TEST_CASE("MaterialLibrary parses the glow props (hue/saturation/strength)") {
  // Real spellings: glow_hue / glow_saturation are all-lower; color_glowStr has
  // a capital S (474 occurrences) with one lowercase color_glowstr straggler.
  const char* xml = R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="p1">
    <material name="speckle">
      <properties>
        <property type="BitMap" name="color_glow_map" value="t\speckle_glow" />
        <property type="Float" name="glow_hue" value="0.05" />
        <property type="Float" name="glow_saturation" value="0.9" />
        <property type="Float" name="color_glowStr" value="10.0" />
      </properties>
    </material>
    <material name="lowercase">
      <properties>
        <property type="Float" name="color_glowstr" value="2.0" />
      </properties>
    </material>
    <material name="off">
      <properties>
        <property type="BitMap" name="color_glow_map" value="t\win_glow" />
        <property type="Float" name="color_glowStr" value="0.0" />
      </properties>
    </material>
    <material name="plain">
      <properties><property type="BitMap" name="color_glow_map" value="t\win_glow" /></properties>
    </material>
  </collection>
</materiallibrary>)";
  const MaterialLibrary lib{std::string(xml)};

  const auto speckle = lib.getMaterial("p1.speckle");
  REQUIRE(speckle.has_value());
  CHECK(speckle->emissiveMap == "t\\speckle_glow");
  CHECK(speckle->glowHue == doctest::Approx(0.05F));
  CHECK(speckle->glowSaturation == doctest::Approx(0.9F));
  CHECK(speckle->glowStr == doctest::Approx(10.0F));

  const auto lower = lib.getMaterial("p1.lowercase");
  REQUIRE(lower.has_value());
  CHECK(lower->glowStr == doctest::Approx(2.0F));

  // 31 real materials ship color_glowStr 0.0 — glow explicitly OFF, the value
  // must survive as 0, not collapse to the 1.0 default.
  const auto off = lib.getMaterial("p1.off");
  REQUIRE(off.has_value());
  CHECK(off->glowStr == doctest::Approx(0.0F));

  // Absent props -> the engine defaults from the shader spec XML
  // (shadergl/ogl/p2_complex_surface.xml): hue 0.5 (identity), sat 1, str 1.
  const auto plain = lib.getMaterial("p1.plain");
  REQUIRE(plain.has_value());
  CHECK(plain->glowHue == doctest::Approx(0.5F));
  CHECK(plain->glowSaturation == doctest::Approx(1.0F));
  CHECK(plain->glowStr == doctest::Approx(1.0F));
}

TEST_CASE("MaterialLibrary parses the Smoothness/Metallness scalar props") {
  const char* xml = R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="p1">
    <material name="hull">
      <properties>
        <property type="BitMap" name="metal_map" value="t\hull_metal" />
        <property type="Float" name="Smoothness" value="0.9" />
        <property type="Float" name="Metallness" value="0.1" />
      </properties>
    </material>
    <material name="lowercase">
      <properties>
        <property type="Float" name="smoothness" value="0.5" />
        <property type="Float" name="metalness" value="0.2" />
      </properties>
    </material>
    <material name="legacyprops">
      <properties>
        <property type="Float" name="mat_smoothness" value="0.3" />
        <property type="Float" name="mat_metallness" value="0.3" />
        <property type="Float" name="smoothnessMax" value="0.3" />
      </properties>
    </material>
    <material name="dual">
      <properties>
        <property type="Float" name="Metallness" value="0.8" />
        <property type="Float" name="metallness" value="0.2" />
      </properties>
    </material>
    <material name="plain">
      <properties><property type="BitMap" name="diffuse_map" value="t\d" /></properties>
    </material>
  </collection>
</materiallibrary>)";
  const MaterialLibrary lib{std::string(xml)};

  const auto hull = lib.getMaterial("p1.hull");
  REQUIRE(hull.has_value());
  REQUIRE(hull->smoothness.has_value());
  CHECK(*hull->smoothness == doctest::Approx(0.9F));
  REQUIRE(hull->metallness.has_value());
  CHECK(*hull->metallness == doctest::Approx(0.1F));

  // Character/older collections ship lowercase spellings; metalness (one l)
  // is a real third alias (survey 2026-07-11: 307/270/37 occurrences).
  const auto lower = lib.getMaterial("p1.lowercase");
  REQUIRE(lower.has_value());
  REQUIRE(lower->smoothness.has_value());
  CHECK(*lower->smoothness == doctest::Approx(0.5F));
  REQUIRE(lower->metallness.has_value());
  CHECK(*lower->metallness == doctest::Approx(0.2F));

  // mat_* feed only the legacy Strauss macros (xu_* shaders), smoothnessMax
  // other shaders — never U_smoothness/U_metallness. Must stay absent.
  const auto legacy = lib.getMaterial("p1.legacyprops");
  REQUIRE(legacy.has_value());
  CHECK_FALSE(legacy->smoothness.has_value());
  CHECK_FALSE(legacy->metallness.has_value());

  // One real material carries two casings of the same prop; document order,
  // last one wins (matches the existing alias-chain behavior).
  const auto dual = lib.getMaterial("p1.dual");
  REQUIRE(dual.has_value());
  REQUIRE(dual->metallness.has_value());
  CHECK(*dual->metallness == doctest::Approx(0.2F));

  // Absent props stay nullopt — presence drives the no-map constant bake.
  const auto plain = lib.getMaterial("p1.plain");
  REQUIRE(plain.has_value());
  CHECK_FALSE(plain->smoothness.has_value());
  CHECK_FALSE(plain->metallness.has_value());
}

TEST_CASE("MaterialLibrary keeps every property in a lower-cased bag") {
  const char* xml = R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="newplanets">
    <material name="planet" shader="p1_planet_earthlike" blendmode="NONE">
      <properties>
        <property type="Float" name="AnisoX" value="0.8" />
        <property type="Integer" name="dblend1" value="2" />
        <property type="Float" name="broken" value="nan" />
        <property type="Float" name="novalue" />
        <property type="Color" name="color_Atmo" r="156" g="300" b="-4" />
        <property type="BitMap" name="Cloudmap_map" value="assets\tex\clouds" />
        <property type="Float" name="" value="1" />
      </properties>
    </material>
  </collection>
</materiallibrary>)";
  const MaterialLibrary lib{std::string(xml)};
  const auto planet = lib.getMaterial("newplanets.planet");
  REQUIRE(planet.has_value());
  const auto& props = planet->properties;
  REQUIRE(props.floats.count("anisox") == 1);
  CHECK(props.floats.at("anisox") == doctest::Approx(0.8F));
  CHECK(props.floats.at("dblend1") == doctest::Approx(2.0F));
  // Hostile values drop the property so the shader default applies.
  CHECK(props.floats.count("broken") == 0);
  CHECK(props.floats.count("novalue") == 0);
  CHECK(props.floats.size() == 2);
  // Channels clamp to the authored 0..255 range; a missing alpha is opaque.
  REQUIRE(props.colors.count("color_atmo") == 1);
  CHECK(props.colors.at("color_atmo") == std::array<float, 4>{156.0F, 255.0F, 0.0F, 255.0F});
  CHECK(props.bitmaps.at("cloudmap_map") == R"(assets\tex\clouds)");
}

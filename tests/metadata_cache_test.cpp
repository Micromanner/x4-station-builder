#include "x4sb/assetpipe/metadata_cache.hpp"

#include "x4sb/archive/archive.hpp"
#include "x4sb/platform/platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <fstream>

namespace {

struct CacheFixture {
  std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("x4sb_metadata_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  CacheFixture() {
    std::filesystem::create_directories(root);
    std::ofstream(root / "01.cat") << "asset 1 1 aa\n";
    std::ofstream(root / "01.dat") << "a";
  }
  ~CacheFixture() {
    std::error_code ec;
    for (const auto* file : {"01.cat", "01.dat", "02.cat", "02.dat", "catalog.json"})
      std::filesystem::remove(root / file, ec);
    std::filesystem::remove(root, ec);
  }
};

}  // namespace

TEST_CASE("material cache preserves optional scalars and nondefault shader inputs") {
  const x4sb::MaterialLibrary original(R"(<materiallibrary><collection name="test">
    <material name="hull" blendmode="ALPHA8"><properties>
      <property name="diffuse_map" type="BitMap" value="assets/hull.dds"/>
      <property name="smoothness" type="Float" value="0"/>
      <property name="glow_hue" type="Float" value="0.05"/>
      <property name="glow_saturation" type="Float" value="0.7"/>
      <property name="normal_detail_tiling" type="Float" value="3"/>
    </properties></material>
    <material name="default"><properties/></material>
  </collection></materiallibrary>)");
  const auto cached = x4sb::MaterialLibrary::fromCacheJson(original.toCacheJson());
  REQUIRE(cached);
  REQUIRE(cached->size() == 2);
  REQUIRE(cached->getMaterial("test.hull"));
  const auto material = *cached->getMaterial("test.hull");
  CHECK(material.diffuseMap == "assets/hull.dds");
  CHECK(material.blendMode == "ALPHA8");
  REQUIRE(material.smoothness);
  CHECK(*material.smoothness == 0.0F);
  CHECK_FALSE(material.metallness);
  CHECK(material.glowHue == doctest::Approx(0.05F));
  CHECK(material.glowSaturation == doctest::Approx(0.7F));
  CHECK(material.normalDetailTiling == 3.0F);
  CHECK(cached->toCacheJson() == original.toCacheJson());
  CHECK_FALSE(x4sb::MaterialLibrary::fromCacheJson(R"({"test":{"glowHue":"bad"}})"));
}

TEST_CASE("material cache round trips the property bag and rejects out-of-range values") {
  const x4sb::MaterialLibrary original(R"(<materiallibrary><collection name="p">
    <material name="planet" shader="p1_planet_gas"><properties>
      <property name="Samples" type="Float" value="3"/>
      <property name="color_atmo" type="Color" r="5" g="40" b="28" a="255"/>
      <property name="polar_map" type="BitMap" value="assets/polar"/>
    </properties></material>
  </collection></materiallibrary>)");
  const auto cached = x4sb::MaterialLibrary::fromCacheJson(original.toCacheJson());
  REQUIRE(cached);
  const auto planet = cached->getMaterial("p.planet");
  REQUIRE(planet);
  CHECK(planet->shader == "p1_planet_gas");
  CHECK(planet->properties.floats.at("samples") == 3.0F);
  CHECK(planet->properties.colors.at("color_atmo") ==
        std::array<float, 4>{5.0F, 40.0F, 28.0F, 255.0F});
  CHECK(planet->properties.bitmaps.at("polar_map") == "assets/polar");
  CHECK(cached->toCacheJson() == original.toCacheJson());
  CHECK_FALSE(
      x4sb::MaterialLibrary::fromCacheJson(R"({"p":{"properties":{"floats":{"x":1e39}}}})"));
  CHECK_FALSE(
      x4sb::MaterialLibrary::fromCacheJson(R"({"p":{"properties":{"colors":{"c":[0,0,0,256]}}}})"));
  CHECK_FALSE(x4sb::MaterialLibrary::fromCacheJson(R"({"p":{"properties":{"bitmaps":{"m":1}}}})"));
}

TEST_CASE("metadata cache round trips and invalidates on archive identity changes") {
  CacheFixture fixture;
  x4sb::Archive archive;
  REQUIRE(archive.addInstall(fixture.root.string()) == 1);
  const auto fingerprint = x4sb::metadataFingerprint(archive);
  REQUIRE(fingerprint);
  x4sb::CachedMetadata metadata;
  x4sb::ModuleDef module;
  module.id = "module";
  module.name = "Localized module";
  x4sb::MeshRef mesh;
  mesh.xmfPath = "assets/part-lod0.xmf";
  module.meshRefs.push_back(mesh);
  metadata.catalog.add(module);
  const auto file = fixture.root / "catalog.json";
  REQUIRE(x4sb::writeMetadataCache(file, *fingerprint, metadata));
  const auto read = x4sb::readMetadataCache(file, *fingerprint);
  REQUIRE(read);
  REQUIRE(read->catalog.find("module"));
  CHECK(read->catalog.find("module")->name == "Localized module");
  CHECK(read->catalog.find("module")->meshRefs[0].xmfPath == mesh.xmfPath);
  CHECK_FALSE(x4sb::readMetadataCache(file, *fingerprint + "new schema"));
  const auto timestamp = std::filesystem::last_write_time(fixture.root / "01.cat");
  std::filesystem::last_write_time(fixture.root / "01.cat", timestamp + std::chrono::seconds(2));
  const auto changedCat = x4sb::metadataFingerprint(archive);
  REQUIRE(changedCat);
  CHECK(*changedCat != *fingerprint);
  CHECK_FALSE(x4sb::readMetadataCache(file, *changedCat));
  std::filesystem::last_write_time(fixture.root / "01.cat", timestamp);
  std::ofstream(fixture.root / "01.dat", std::ios::app) << "b";
  const auto changedDat = x4sb::metadataFingerprint(archive);
  REQUIRE(changedDat);
  CHECK(*changedDat != *fingerprint);
  CHECK_FALSE(x4sb::readMetadataCache(file, *changedDat));
  // Atomic replacement must work with an existing cache, including on Windows.
  REQUIRE(x4sb::writeMetadataCache(file, *changedDat, metadata));
  REQUIRE(x4sb::readMetadataCache(file, *changedDat));
  std::ofstream(fixture.root / "02.cat") << "other 1 1 aa\n";
  std::ofstream(fixture.root / "02.dat") << "c";
  x4sb::Archive updated;
  REQUIRE(updated.addInstall(fixture.root.string()) == 2);
  REQUIRE(x4sb::metadataFingerprint(updated));
  CHECK(*x4sb::metadataFingerprint(updated) != *changedDat);
  REQUIRE(x4sb::platform::writeFileAtomically(file, "{truncated"));
  CHECK_FALSE(x4sb::readMetadataCache(file, *changedDat));
}

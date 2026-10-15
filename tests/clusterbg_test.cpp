#include "x4sb/assetpipe/clusterbg.hpp"

#include "x4sb/archive/archive.hpp"
#include "x4sb/assetpipe/material_library.hpp"
#include "x4sb/assetpipe/native_background.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace x4sb;

namespace {

const char* kCluster11Xml = R"(<?xml version="1.0"?>
<components>
  <component name="Cluster_11">
    <layers>
      <layer>
        <lights>
          <star intensity="1.5" r="255" g="200" b="150" x="1" y="2" z="3" range="1.85" specularintensity="0.375"/>
          <star intensity="2.0" r="100" g="100" b="255" x="-1" y="0" z="0"/>
          <omni intensity="1.2" lenseffect="sun_flare" r="255" g="255" b="255">
            <offset><position x="10" y="10" z="10"/></offset>
          </omni>
          <ambient r="10" g="20" b="30"/>
        </lights>
      </layer>
    </layers>
    <connections>
      <connection name="c1">
        <offset>
          <position x="0" y="0" z="100"/>
          <quaternion qw="1" qx="0" qy="0" qz="0"/>
        </offset>
        <parts>
          <part name="bg_1">
            <size><max x="50" y="100" z="0"/></size>
            <lods>
              <lod index="0">
                <materials>
                  <material id="1" ref="environments.cluster_11_bg"/>
                </materials>
              </lod>
            </lods>
          </part>
        </parts>
      </connection>
      <connection name="c2">
        <offset><position x="0" y="1" z="0"/></offset>
        <parts>
          <part name="bg_2">
          </part>
        </parts>
      </connection>
    </connections>
  </component>
</components>)";

const char* kMaterialLibraryXml = R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="environments">
    <material name="cluster_11_bg">
      <properties>
        <property name="diffuse_map" type="BitMap" value="assets\environments\cluster\textures\cluster_11_bg" />
      </properties>
    </material>
  </collection>
</materiallibrary>)";

const unsigned char kMockGz[] = {
    0x1f, 0x8b, 0x08, 0x00,  // magic, deflate
    0x00, 0x00, 0x00, 0x00,  // mtime
    0x00, 0x00,              // xfl, os
    // deflate block (uncompressed)
    0x01,                // BFINAL=1, BTYPE=00
    0x04, 0x00,          // LEN = 4
    0xFB, 0xFF,          // NLEN = ~4
    'D', 'D', 'S', ' ',  // data
    // footer
    0x00, 0x00, 0x00, 0x00,  // CRC32 (ignored)
    0x04, 0x00, 0x00, 0x00   // ISIZE = 4
};

}  // namespace

TEST_CASE("native texture reads normalize source extensions and inflate gzip without transcoding") {
  const ExtractFn extract = [](const std::string& path) -> std::optional<std::string> {
    if (path == "assets/atlas.gz")
      return std::string(reinterpret_cast<const char*>(kMockGz), sizeof(kMockGz));
    return std::nullopt;
  };
  CHECK(readTextureDds(extract, "ASSETS\\ATLAS.tga") == std::optional<std::string>("DDS "));
  CHECK(readTextureDds(extract, "assets/atlas.dds") == std::optional<std::string>("DDS "));
  CHECK_FALSE(readTextureDds(extract, "missing"));
}

TEST_CASE("native background metadata resolves DLC clusters without extracting textures") {
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / "x4sb_native_background_test";
  fs::create_directories(dir);
  const std::string xml = kCluster11Xml;
  {
    std::ofstream(dir / "01.cat") << "assets/environments/clusters/Cluster_11.xml " << xml.size()
                                  << " 1 aa\n";
    std::ofstream(dir / "01.dat", std::ios::binary) << xml;
  }
  Archive archive;
  REQUIRE(archive.addCatalog((dir / "01.cat").string(), "extensions/test/"));
  const MaterialLibrary materials(kMaterialLibraryXml);
  REQUIRE(assetpipe::clusterComponentPath(archive, "cluster_11"));
  CHECK(*assetpipe::clusterComponentPath(archive, "cluster_11") ==
        "extensions/test/assets/environments/clusters/cluster_11.xml");
  CHECK_FALSE(assetpipe::clusterComponentPath(archive, "cluster_11/../bad"));
  const auto metadata = assetpipe::nativeBackgroundManifest(archive, materials, "cluster_11");
  REQUIRE(metadata);
  const auto json = nlohmann::json::parse(*metadata);
  CHECK(json["stars"].size() == 2);
  CHECK(json["starAtlas"] == assetpipe::kStarAtlasSource);
  CHECK_FALSE(json.contains("planes"));  // sky parts are dome meshes, never billboards
  CHECK(json["sun"]["dir"][2].get<double>() < 0);
  CHECK(json["ambient"]["color"][1] == 20);
  fs::remove(dir / "01.cat");
  fs::remove(dir / "01.dat");
  fs::remove(dir);
}

TEST_CASE("extractClusterBackground parses stars and sun") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "x4sb_clusterbg_test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  auto writePair = [&](const std::string& name, const std::string& cat, const std::string& dat) {
    std::ofstream(dir / (name + ".cat"), std::ios::binary) << cat;
    std::ofstream(dir / (name + ".dat"), std::ios::binary) << dat;
  };

  std::string datStr;
  std::string catStr;

  auto addFile = [&](const std::string& path, const std::string& content) {
    catStr += path + " " + std::to_string(content.size()) + " 1 aa\n";
    datStr += content;
  };

  addFile("assets/environments/cluster/cluster_11.xml", kCluster11Xml);
  addFile("libraries/material_library.xml", kMaterialLibraryXml);
  addFile("assets/environments/cluster/textures/cluster_11_bg.gz",
          std::string(reinterpret_cast<const char*>(kMockGz), sizeof(kMockGz)));

  writePair("01", catStr, datStr);

  Archive ar;
  REQUIRE(ar.addCatalog((dir / "01.cat").string()));

  auto bgOpt = assetpipe::extractClusterBackground(ar, 11);
  REQUIRE(bgOpt.has_value());
  auto& bg = *bgOpt;

  // Stars
  REQUIRE(bg.stars.size() == 2);
  CHECK(bg.stars[0].brightness == doctest::Approx(1.5f));
  CHECK(bg.stars[0].range == doctest::Approx(1.85f));
  CHECK(bg.stars[0].specularIntensity == doctest::Approx(0.375f));
  CHECK(bg.stars[1].range == doctest::Approx(1.0f));  // absent -> p1_star default radius
  CHECK(bg.stars[1].specularIntensity == doctest::Approx(0.0f));
  CHECK(bg.stars[0].color.r == 255);
  CHECK(bg.stars[0].color.g == 200);
  CHECK(bg.stars[0].color.b == 150);
  // dir flipped check (x=1,y=2,z=3 -> normalized and z flipped)
  double len = std::sqrt(14.0);
  CHECK(bg.stars[0].dir.x == doctest::Approx(1.0 / len));
  CHECK(bg.stars[0].dir.y == doctest::Approx(2.0 / len));
  CHECK(bg.stars[0].dir.z == doctest::Approx(-3.0 / len));

  // Sun
  CHECK(bg.sun.intensity == doctest::Approx(1.2f));
  CHECK(bg.sun.lenseffect == "sun_flare");
  CHECK(bg.sun.color.r == 255);
  CHECK(bg.sun.distance == doctest::Approx(std::sqrt(300.0)));  // |(10,10,10)|
  CHECK(bg.ambient.color.b == 30);
  CHECK(bg.ambient.intensity == doctest::Approx(1.0f));  // absent -> 1

  // Seen from a sector at (10,0,10) the omni is straight "up": (0,10,0).
  const auto fromSector = assetpipe::extractClusterBackground(ar, 11, "", {10, 0, 10});
  REQUIRE(fromSector);
  CHECK(fromSector->sun.dir.x == doctest::Approx(0.0));
  CHECK(fromSector->sun.dir.y == doctest::Approx(1.0));
  CHECK(fromSector->sun.dir.z == doctest::Approx(0.0));
  CHECK(fromSector->sun.distance == doctest::Approx(10.0));

  fs::remove_all(dir);
}

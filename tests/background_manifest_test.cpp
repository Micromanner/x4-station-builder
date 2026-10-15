#include "x4sb/assetpipe/clusterbg.hpp"
#include "x4sb/assetpipe/component.hpp"
#include "x4sb/assetpipe/material_library.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

using namespace x4sb;
using namespace x4sb::assetpipe;

namespace {

// Synthetic geometry modelled on a real Cluster_NN.xml (NOT extracted game data):
// one huge vertex-color dome, one huge additive star sphere, one planet-sized
// part that must NOT be selected, one tiny station-scale part, one sub-1e8 nebula
// sheet far out (Cluster_15 nebula001) that must be selected by name.
ComponentGeometry skyGeo() {
  ComponentGeometry geo;
  geo.geometryFolder = "assets/environments/cluster/test_data";
  ComponentPart dome;
  dome.name = "part_c99_bg";
  dome.materialRef = "p1.p1_env_bg_vertexcolor";
  dome.offset.position = {1e6, 0, 2e6};
  dome.sizeMax = {4.9e8, 4.9e8, 4.9e8};
  ComponentPart stars;
  stars.name = "Background";
  stars.materialRef = "p1.backgroundstars_v2";
  stars.offset.rotation = {0.9975, 0.0, -0.0697, 0.0};  // {w,x,y,z}
  stars.sizeMax = {4.2e8, 4.2e8, 4.2e8};
  ComponentPart planet;
  planet.name = "gasgiant";
  planet.materialRef = "newplanets.planet_test";
  planet.sizeMax = {5e7, 5e7, 5e7};
  ComponentPart tiny;
  tiny.name = "part_main";
  tiny.sizeMax = {30, 30, 30};
  ComponentPart sheet;
  sheet.name = "nebula001";
  sheet.materialRef = "p1.staratlas_additive";
  sheet.offset.position = {0, 0, 8.1e7};
  sheet.sizeMax = {9.8e7, 9.2e7, 1e6};
  geo.parts = {dome, stars, planet, tiny, sheet};
  return geo;
}

MaterialLibrary skyMats() {
  return MaterialLibrary{std::string(R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="p1">
    <material name="p1_env_bg_vertexcolor" shader="p1_simple_unlit" blendmode="NONE"><properties/></material>
    <material name="backgroundstars_v2" shader="p1_starfield" blendmode="ADDITIVE"><properties/></material>
    <material name="staratlas_additive" shader="p1_stars" blendmode="ADDITIVE"><properties/></material>
  </collection>
</materiallibrary>)")};
}

}  // namespace

TEST_CASE(
    "selectSkyDomes keeps extent>=1e8 and sky-named parts, maps blend, flips to display space") {
  const MaterialLibrary lib = skyMats();
  const std::vector<ClusterDome> domes = selectSkyDomes(skyGeo(), &lib);
  REQUIRE(domes.size() == 3);  // planet + station part excluded by extent

  CHECK(domes[0].part == "part_c99_bg");
  CHECK(domes[0].additive == false);  // blendmode NONE -> opaque-unlit
  CHECK(domes[0].extent == doctest::Approx(4.9e8));
  // (1,1,-1) display flip: z negated on position...
  CHECK(domes[0].pos.z == doctest::Approx(-2e6));
  CHECK(domes[0].pos.x == doctest::Approx(1e6));

  CHECK(domes[1].part == "Background");
  CHECK(domes[1].additive == true);
  // ...and the conjugated rotation negates x,y quat components.
  CHECK(domes[1].quat.w == doctest::Approx(0.9975));
  CHECK(domes[1].quat.y == doctest::Approx(0.0697));
  CHECK(domes[1].quat.z == doctest::Approx(0.0));

  CHECK(domes[2].part == "nebula001");
  CHECK(domes[2].additive == true);
  CHECK(domes[2].pos.z == doctest::Approx(-8.1e7));
}

TEST_CASE("selectSkyDomes positions domes relative to the viewing sector") {
  const MaterialLibrary lib = skyMats();
  const std::vector<ClusterDome> domes = selectSkyDomes(skyGeo(), &lib, Vec3{8e7, 0, 2.7e7});
  REQUIRE(domes.size() == 3);
  // Cluster-space (1e6, 0, 2e6) seen from the sector, then z negated for display.
  CHECK(domes[0].pos.x == doctest::Approx(1e6 - 8e7));
  CHECK(domes[0].pos.y == doctest::Approx(0.0));
  CHECK(domes[0].pos.z == doctest::Approx(-(2e6 - 2.7e7)));
  CHECK(domes[1].pos.x == doctest::Approx(-8e7));
}

TEST_CASE("backgroundManifestJson: domes present, no billboard planes") {
  ClusterBackground bg;
  bg.sun.dir = {0, 1, 0};
  bg.sun.color = {255, 250, 240, 255};
  bg.sun.intensity = 1.5f;
  bg.stars.push_back(Star{{0, 0, 1}, {200, 200, 220, 255}, 0.8f});

  ClusterDome dome;
  dome.part = "part_c99_bg";
  dome.xmfPath = "assets/environments/cluster/cluster_99_data/part_c99_bg-lod0.xmf";
  dome.materialRef = "environments.c99_bg";
  dome.additive = false;
  dome.pos = {1e6, 0, -2e6};
  dome.quat = {1, 0, 0, 0};
  dome.extent = 4.9e8;

  const std::string json = backgroundManifestJson(bg, {dome}, {});
  const nlohmann::json doc = nlohmann::json::parse(json);

  // Existing reader contract (sky.cpp): sun/stars shapes unchanged.
  CHECK(doc["sun"]["intensity"] == doctest::Approx(1.5));
  REQUIRE(doc["stars"].size() == 1);
  CHECK(doc["stars"][0]["brightness"] == doctest::Approx(0.8));
  CHECK_FALSE(doc.contains("planes"));

  REQUIRE(doc["domes"].size() == 1);
  CHECK(doc["domes"][0]["xmf"] ==
        "assets/environments/cluster/cluster_99_data/part_c99_bg-lod0.xmf");
  CHECK(doc["domes"][0]["material"] == "environments.c99_bg");
  CHECK(doc["domes"][0]["blend"] == "opaque");
  CHECK(doc["domes"][0]["pos"][2] == doctest::Approx(-2e6));
  CHECK(doc["domes"][0]["quat"][3] == doctest::Approx(1.0));  // [x,y,z,w] order
  CHECK(doc["domes"][0]["extent"] == doctest::Approx(4.9e8));
}

TEST_CASE("backgroundManifestJson: empty background yields empty domes array") {
  ClusterBackground bg;
  const std::string json = backgroundManifestJson(bg, {}, {});
  const nlohmann::json doc = nlohmann::json::parse(json);
  CHECK(doc["domes"].is_array());
}

TEST_CASE("backgroundManifestJson writes sun.elements when sprites are present") {
  using namespace x4sb::assetpipe;
  ClusterBackground bg;
  bg.sun.dir = {0, 0, 1};
  bg.sun.color = {255, 250, 240, 255};
  bg.sun.intensity = 1.1f;
  bg.sun.lenseffect = "sun_ol";
  bg.sun.distance = 3.64e8;
  SunSprite s;
  s.texFile = "lensflares/fx_diff.dds";
  s.mesh.xy = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
  s.mesh.uv = {0.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f};
  s.mesh.rgb = {255, 255, 255, 231, 200, 135, 0, 0, 0};
  s.mesh.index = {0, 1, 2};
  s.screenMode = false;
  s.size = 0.78f;
  s.rotation = -2.0f;
  s.glow = true;
  s.shineBehind = true;
  s.intensity = 1.6f;
  s.screenBlend = true;
  s.grade = {1.3f, 1.0f, 1.5f, 20.0f};
  const std::string json = backgroundManifestJson(bg, {}, {s});
  const auto doc = nlohmann::json::parse(json);
  CHECK(doc["sun"]["lenseffect"] == "sun_ol");
  CHECK(doc["sun"]["distance"].get<double>() == doctest::Approx(3.64e8));
  REQUIRE(doc["sun"]["elements"].size() == 1);
  const auto& e = doc["sun"]["elements"][0];
  CHECK(e["tex"] == "lensflares/fx_diff.dds");
  CHECK(e["mesh"]["xy"].size() == 6);
  CHECK(e["mesh"]["uv"][5].get<double>() == doctest::Approx(0.0));
  CHECK(e["mesh"]["rgb"][3] == 231);
  CHECK(e["mesh"]["index"] == nlohmann::json::array({0, 1, 2}));
  CHECK(e["sizeMode"] == "angular");
  CHECK(e["size"].get<double>() == doctest::Approx(0.78));
  CHECK(e["rotation"].get<double>() == doctest::Approx(-2.0));
  CHECK(e["glow"] == true);
  CHECK(e["shineBehind"] == true);
  CHECK(e["intensity"].get<double>() == doctest::Approx(1.6));
  CHECK(e["screenBlend"] == true);
  REQUIRE(e["grade"].size() == 4);
  CHECK(e["grade"][0].get<double>() == doctest::Approx(1.3));
  CHECK(e["grade"][3].get<double>() == doctest::Approx(20.0));
}

TEST_CASE("backgroundManifestJson omits sun.elements when no sprites") {
  using namespace x4sb::assetpipe;
  ClusterBackground bg;
  const std::string json = backgroundManifestJson(bg, {}, {});
  const auto doc = nlohmann::json::parse(json);
  CHECK_FALSE(doc["sun"].contains("elements"));
  CHECK_FALSE(doc["sun"].contains("lenseffect"));
}

namespace {

// Synthetic planet system modelled on real Cluster_01 / cluster_421 / cluster_107
// shapes (NOT extracted game data). Names are deliberately misleading: the game's
// are too ("Tube01" ring, a moon called "planet").
ComponentGeometry planetGeo() {
  ComponentGeometry geo;
  geo.geometryFolder = "assets/environments/cluster/test_data";
  const auto part = [&geo](const char* name, const char* material, Vec3 pos, double size) {
    ComponentPart p;
    p.name = name;
    p.materialRef = material;
    p.offset.position = pos;
    p.sizeMax = {size, size, size};
    geo.parts.push_back(p);
    return geo.parts.size() - 1;
  };
  part("part_c99_bg", "p1.p1_env_bg_vertexcolor", {0, 0, 0}, 4.9e8);
  part("Sphere001", "planets.gas", {1.25e8, 1.9e7, 1.48e8}, 5e7);
  part("atmo", "planets.atmo", {1.25e8, 1.9e7, 1.48e8}, 5.12e7);
  part("Tube01", "planets.rings", {1.25e8, 1.9e7, 1.48e8}, 2e8);  // extent >= 1e8: was a dome
  part("part_smoke", "planets.rings", {0, 5e8, 0}, 2e8);          // ALPHA8, no body: scenery
  const auto moon = part("planet", "planets.moon", {-8e7, 0, 0}, 1.63e8);
  geo.parts[moon].sizeRaw = {1.56e6, 1.56e6, 1.56e6};  // orbiting: size is the swept box
  part("stray_atmo", "planets.atmo", {0, 0, 7.3e7}, 5e7);
  part("part_sun", "planets.sun", {-3e8, 2e8, -1.5e8}, 3e7);
  part("Loft205", "planets.flares", {-3.02e8, 2e8, -1.5e8}, 3e7);  // pivot 0.07 sun radii off
  part("far_glow", "planets.flares", {0, -4e8, 0}, 3e7);           // ADDITIVE, no body: scenery
  // Terran-style attachment: no offset of its own; the parser composes its
  // connection parent (the moon) into the offset.
  part("moon_atmo", "planets.atmo", {-8e7, 0, 0}, 1.8e6);
  return geo;
}

MaterialLibrary planetMats() {
  return MaterialLibrary{std::string(R"(<?xml version="1.0"?>
<materiallibrary>
  <collection name="p1">
    <material name="p1_env_bg_vertexcolor" shader="p1_simple_unlit" blendmode="NONE"><properties/></material>
  </collection>
  <collection name="planets">
    <material name="gas" shader="p1_planet_gas_mv" blendmode="NONE"><properties/></material>
    <material name="atmo" shader="p1_planet_atmosphere_scattering" blendmode="PLANET_ATMOSPHERE"><properties/></material>
    <material name="rings" shader="p1_complex_unlit" blendmode="ALPHA8"><properties/></material>
    <material name="flares" shader="p1_complex_unlit" blendmode="ADDITIVE"><properties/></material>
    <material name="moon" shader="p1_planet_moon" blendmode="NONE"><properties/></material>
    <material name="sun" shader="p1_planet_sun" blendmode="NONE"><properties/></material>
  </collection>
</materiallibrary>)")};
}

}  // namespace

TEST_CASE("selectSkyDomes drops planet-system parts even when extent >= 1e8") {
  const MaterialLibrary lib = planetMats();
  const std::vector<ClusterDome> domes = selectSkyDomes(planetGeo(), &lib);
  REQUIRE(domes.size() == 2);  // the nebula dome + the unpaired ALPHA8 smoke
  CHECK(domes[0].part == "part_c99_bg");
  CHECK(domes[1].part == "part_smoke");
}

TEST_CASE("selectPlanetParts classifies by shader, pairs attachments, views from the sector") {
  const MaterialLibrary lib = planetMats();
  const std::vector<PlanetPart> parts = selectPlanetParts(planetGeo(), lib, {1e7, 0, 2e7});
  REQUIRE(parts.size() == 8);  // far_glow sits on no body
  // Bodies (suns included) first, in component order.
  CHECK(parts[0].part == "Sphere001");
  CHECK(parts[0].kind == PlanetPartKind::Planet);
  CHECK(parts[1].part == "planet");
  CHECK(parts[1].kind == PlanetPartKind::Moon);
  CHECK(parts[1].radius == doctest::Approx(1.56e6));  // size_raw, not the orbit box
  CHECK(parts[2].part == "part_sun");
  CHECK(parts[2].kind == PlanetPartKind::Sun);
  CHECK(parts[3].part == "atmo");
  CHECK(parts[3].kind == PlanetPartKind::Atmosphere);
  CHECK(parts[3].body == 0);
  CHECK(parts[4].part == "Tube01");
  CHECK(parts[4].kind == PlanetPartKind::Ring);
  CHECK(parts[4].body == 0);
  CHECK(parts[5].part == "stray_atmo");
  CHECK(parts[5].body == -1);  // sits on no body (the real cluster_723 stray)
  CHECK(parts[6].part == "Loft205");
  CHECK(parts[6].kind == PlanetPartKind::Flare);
  CHECK(parts[6].body == 2);
  CHECK(parts[7].part == "moon_atmo");
  CHECK(parts[7].body == 1);
  CHECK(parts[7].pos.x == doctest::Approx(-9e7));
  // Sector-relative, then the (1,1,-1) display flip.
  CHECK(parts[0].pos.x == doctest::Approx(1.15e8));
  CHECK(parts[0].pos.y == doctest::Approx(1.9e7));
  CHECK(parts[0].pos.z == doctest::Approx(-1.28e8));
  CHECK(parts[0].radius == doctest::Approx(5e7));
}

TEST_CASE("backgroundManifestJson writes the planets array") {
  const MaterialLibrary lib = planetMats();
  std::vector<PlanetPart> parts = selectPlanetParts(planetGeo(), lib);
  parts[0].xmfPath = "assets/environments/cluster/test_data/sphere001-lod0.xmf";
  const nlohmann::json j = nlohmann::json::parse(backgroundManifestJson({}, {}, {}, parts));
  REQUIRE(j["planets"].size() == 8);
  CHECK(j["planets"][0]["kind"] == "planet");
  CHECK(j["planets"][0]["xmf"] == "assets/environments/cluster/test_data/sphere001-lod0.xmf");
  CHECK(j["planets"][0]["material"] == "planets.gas");
  CHECK(j["planets"][2]["kind"] == "sun");
  CHECK(j["planets"][4]["kind"] == "ring");
  CHECK(j["planets"][4]["body"] == 0);
  CHECK(j["planets"][6]["kind"] == "flare");
  CHECK(j["planets"][1]["radius"].get<double>() == doctest::Approx(1.56e6));
  CHECK(nlohmann::json::parse(backgroundManifestJson({}, {}, {}))["planets"].empty());
}

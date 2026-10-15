#include "x4sb/assetpipe/nebula_fog.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>

using namespace x4sb;
using namespace x4sb::assetpipe;

namespace {

constexpr const char* kClusters = R"(<macros>
  <macro name="Cluster_07_macro" class="cluster">
    <connections>
      <connection name="r1" ref="regions">
        <macro name="r1_macro"><properties><region ref="ball" /></properties></macro>
      </connection>
      <connection name="r2" ref="regions">
        <offset>
          <position x="1000" y="0" z="0" />
          <quaternion qx="0" qy="0.7071068" qz="0" qw="0.7071068" />
        </offset>
        <macro name="r2_macro"><properties><region ref="tube_alias" /></properties></macro>
      </connection>
      <!--connection name="r3" ref="regions">
        <macro name="r3_macro"><properties><region ref="ball" /></properties></macro>
      </connection-->
      <connection name="s" ref="sectors"><macro ref="Cluster_07_Sector001_macro" /></connection>
    </connections>
  </macro>
  <macro name="Cluster_08_macro" class="cluster">
    <connections>
      <connection name="r1" ref="regions">
        <macro name="x"><properties><region ref="other" /></properties></macro>
      </connection>
    </connections>
  </macro>
</macros>)";

constexpr const char* kRegions = R"(<regions>
  <region name="ball" density="1.0">
    <boundary class="sphere"><size r="500000" /></boundary>
    <fields>
      <volumetricfog multiplier="0.2" medium="haze" texture="tex/a" size="100000" seed="7" />
    </fields>
  </region>
  <region name="tube" density="1.5" minnoisevalue="0" maxnoisevalue="1">
    <boundary class="splinetube">
      <size r="4000" />
      <splineposition x="0" y="0" z="0" tx="1" ty="0" tz="0" inlength="0" outlength="100000" />
      <splineposition x="300000" y="0" z="0" tx="1" ty="0" tz="0" inlength="100000" outlength="0" />
    </boundary>
    <falloff>
      <lateral><step position="0" value="1" /><step position="1" value="1" /></lateral>
    </falloff>
    <fields>
      <volumetricfog multiplier="0.3" medium="glow" texture="tex/b" size="23000" sizevariation="0.5" seed="9" />
      <volumetricfog multiplier="0.3" medium="unknown" texture="tex/c" size="23000" seed="9" />
    </fields>
  </region>
  <region name="nan" density="nan"><boundary class="sphere"><size r="10" /></boundary></region>
  <region name="flat"><boundary class="sphere"><size r="-5" /></boundary></region>
  <alias name="tube_alias" ref="tube" />
  <alias name="loop_a" ref="loop_b" />
  <alias name="loop_b" ref="loop_a" />
</regions>)";

constexpr const char* kMedia = R"(<library>
  <medium name="haze" scale="1.5925e-06">
    <absorption r="0.001044262317" g="0" b="-1" scale="0.09576138109" />
    <phase function="MEDIUM_PHASEFUNC_HG" r="0" g="0.1852433234" b="1" scale="1" shape="0.163" />
  </medium>
  <medium name="glow" scale="3e-05">
    <absorption r="-0.2349234819" g="-0.506950736" b="-1" scale="0.9998999238" />
    <phase function="MEDIUM_PHASEFUNC_HG" r="0.235" g="0.507" b="1" scale="1" shape="0.1" />
    <phase function="MEDIUM_PHASEFUNC_BOGUS" r="1" g="1" b="1" scale="1" shape="0" />
    <phase function="MEDIUM_PHASEFUNC_ISOTROPIC" r="1" g="1" b="1" scale="0.5" shape="0" />
    <phase function="MEDIUM_PHASEFUNC_RAYLEIGH" r="1" g="1" b="1" scale="0.5" shape="0" />
    <phase function="MEDIUM_PHASEFUNC_MIEHAZY" r="1" g="1" b="1" scale="0.5" shape="0" />
  </medium>
</library>)";

RegionLibrary regions() {
  RegionLibrary lib;
  parseRegionDefinitions(kRegions, lib);
  return lib;
}

MediumLibrary media() {
  MediumLibrary lib;
  parseMediumLibrary(kMedia, lib);
  return lib;
}

}  // namespace

TEST_CASE("parseClusterRegions reads region connections of one cluster") {
  const auto placements = parseClusterRegions(kClusters, "cluster_07_MACRO");
  REQUIRE(placements.size() == 2);
  CHECK(placements[0].region == "ball");
  CHECK(placements[0].offset.x == doctest::Approx(0));
  CHECK(placements[1].region == "tube_alias");
  CHECK(placements[1].offset.x == doctest::Approx(1000));
  CHECK(placements[1].rotation.y == doctest::Approx(0.7071068));
  CHECK(parseClusterRegions(kClusters, "Cluster_09_macro").empty());
  CHECK(parseClusterRegions("<macros", "Cluster_07_macro").empty());
}

TEST_CASE("parseRegionDefinitions keeps valid regions and resolves aliases") {
  const RegionLibrary lib = regions();
  CHECK(lib.regions.size() == 2);  // nan density and negative radius dropped
  const RegionDef* tube = lib.find("tube_alias");
  REQUIRE(tube != nullptr);
  CHECK(tube->boundary == RegionBoundary::SplineTube);
  CHECK(tube->spline.size() == 2);
  CHECK(tube->fogs.size() == 2);
  CHECK(tube->fogs[0].sizeVariation == doctest::Approx(0.5));
  CHECK(lib.find("loop_a") == nullptr);
  CHECK(lib.find("missing") == nullptr);

  RegionLibrary merged = lib;
  parseRegionDefinitions(
      R"(<regions><region name="ball"><boundary class="cylinder"><size r="5" linear="2" />
         </boundary></region><region name="cluster"><fields /></region></regions>)",
      merged);
  CHECK(merged.regions.at("ball").boundary == RegionBoundary::Cylinder);
  CHECK(merged.regions.at("cluster").boundary == RegionBoundary::Unbounded);
}

TEST_CASE("parseMediumLibrary derives X4's scattering and extinction") {
  const MediumLibrary lib = media();
  const FogMedium& haze = lib.at("haze");
  // V_volume_sigma_extinction[0] and phase0_param[0] of X4_Station_FRONT.rdc.
  CHECK(haze.sigmaExt[0] == doctest::Approx(1.5925e-10).epsilon(1e-3));
  CHECK(haze.sigmaExt[1] == doctest::Approx(2.95e-7).epsilon(1e-3));
  CHECK(haze.sigmaExt[2] == doctest::Approx(1.44e-6).epsilon(1e-3));
  REQUIRE(haze.phases.size() == 1);
  CHECK(haze.phases[0].function == PhaseFunction::HenyeyGreenstein);
  CHECK(haze.phases[0].shape == doctest::Approx(0.163));
  CHECK(haze.phases[0].sigmaS[2] == doctest::Approx(1.5925e-6).epsilon(1e-4));

  const FogMedium& glow = lib.at("glow");
  REQUIRE(glow.phases.size() == 3);  // unknown function skipped, 4th phase truncated
  CHECK(glow.phases[1].function == PhaseFunction::Isotropic);
  CHECK(glow.phases[2].function == PhaseFunction::Rayleigh);
  CHECK(glow.phases[0].sigmaS[0] == doctest::Approx(7.05e-6).epsilon(1e-4));
  // Extinction = the three phases' scattering (3.705e-5) + negative absorption (-7.047e-6).
  CHECK(glow.sigmaExt[0] == doctest::Approx(3.0003e-5).epsilon(1e-3));
}

TEST_CASE("bezierPoint hits the end points and stays on a straight spline") {
  const SplinePoint a{{0, 0, 0}, {1, 0, 0}, 0, 10};
  const SplinePoint b{{30, 0, 0}, {1, 0, 0}, 10, 0};
  CHECK(bezierPoint(a, b, 0.0).x == doctest::Approx(0));
  CHECK(bezierPoint(a, b, 1.0).x == doctest::Approx(30));
  CHECK(bezierPoint(a, b, 0.5).x == doctest::Approx(15));
  CHECK(bezierPoint(a, b, 0.5).y == doctest::Approx(0));
}

TEST_CASE("sphere regions get X4's basefill count and radius") {
  const RegionLibrary lib = regions();
  const RegionDef& ball = lib.regions.at("ball");
  const auto spheres = generateFogSpheres(ball, ball.fogs[0]);
  CHECK(spheres.size() == 496);
  for (const FogSphere& s : spheres) {
    CHECK(length(s.pos) <= 500000.0);
    CHECK(s.radius == doctest::Approx(86576));
    CHECK(s.density == doctest::Approx(0.2F));
  }
  const auto again = generateFogSpheres(ball, ball.fogs[0]);
  REQUIRE(again.size() == spheres.size());
  CHECK(again[17].pos.x == spheres[17].pos.x);
}

TEST_CASE("tube regions place spheres along the spline") {
  const RegionLibrary lib = regions();
  const RegionDef& tube = lib.regions.at("tube");
  const auto spheres = generateFogSpheres(tube, tube.fogs[0]);
  // kTubeFill x density x mean lateral x length / size.
  CHECK(spheres.size() == static_cast<std::size_t>(std::round(1.9635 * 1.5 * 300000 / 23000)));
  for (const FogSphere& s : spheres) {
    CHECK(s.pos.x >= -1.0);
    CHECK(s.pos.x <= 300001.0);
    CHECK(std::hypot(s.pos.y, s.pos.z) <= 4000 * 1.2 + 1e-6);
    CHECK(s.radius >= 23000 * kFogRadiusScale * 0.5 - 1e-6);
    CHECK(s.radius <= 23000 * kFogRadiusScale * 1.5 + 1e-6);
  }
  RegionDef renamed = tube;
  renamed.name = "tube2";
  CHECK(generateFogSpheres(renamed, tube.fogs[0])[0].pos.x != spheres[0].pos.x);

  RegionDef huge = tube;
  huge.fogs[0].size = 1e-3;
  CHECK(generateFogSpheres(huge, huge.fogs[0]).size() <= kMaxFogSpheres);
}

TEST_CASE("buildFogField places regions and groups media") {
  const auto field = buildFogField(parseClusterRegions(kClusters, "Cluster_07_macro"), regions(),
                                   media(), {5, 0, 0});
  CHECK(field.origin.x == doctest::Approx(5));
  REQUIRE(field.media.size() == 2);  // "unknown" has no medium
  CHECK(field.media[0].name == "haze");
  CHECK(field.media[0].pattern == "tex/a");
  CHECK(field.media[1].name == "glow");
  std::size_t tubeSpheres = 0;
  for (const FogSphere& s : field.spheres) {
    if (s.medium != 1) continue;
    ++tubeSpheres;
    // The 90 deg yaw turns the +x spline onto -z, shifted 1000 along x.
    CHECK(std::abs(s.pos.x - 1000.0) <= 4800.0 + 1e-6);
    CHECK(s.pos.z <= 4800.0 + 1e-6);
  }
  CHECK(tubeSpheres > 0);
}

TEST_CASE("fog field JSON round-trips and rejects hostile records") {
  const auto field = buildFogField(parseClusterRegions(kClusters, "Cluster_07_macro"), regions(),
                                   media(), {5, 6, 7});
  const auto back = fogFieldFromJson(fogFieldToJson(field));
  CHECK(back.origin.z == doctest::Approx(7));
  REQUIRE(back.media.size() == field.media.size());
  CHECK(back.media[1].phases.size() == 3);
  REQUIRE(back.spheres.size() == field.spheres.size());
  CHECK(back.spheres[3].pos.y == doctest::Approx(field.spheres[3].pos.y));
  CHECK(back.spheres[3].medium == field.spheres[3].medium);

  auto j = fogFieldToJson(field);
  j["spheres"] =
      nlohmann::json::array({{1, 2, 3, 1000, 0.2, "haze"},
                             {1, 2, 3, std::numeric_limits<double>::quiet_NaN(), 0.2, "haze"},
                             {1, 2, 3, 1000, 0.2, "nope"},
                             {1, 2, 3, -5, 0.2, "haze"},
                             {1, 2},
                             "junk"});
  CHECK(fogFieldFromJson(j).spheres.size() == 1);
  CHECK(fogFieldFromJson(nlohmann::json::object()).spheres.empty());
  CHECK(fogSpheresFromJson(nlohmann::json{{"spheres", {{0, 0, 0, 1, 1, "glow"}}}}, back.media)
            .at(0)
            .medium == 1);
}

TEST_CASE("cylinder, box and unbounded regions stay inside their boundary") {
  RegionLibrary lib;
  parseRegionDefinitions(
      R"(<regions>
        <region name="disc"><boundary class="cylinder"><size r="25000" linear="5000" /></boundary>
          <fields><volumetricfog medium="m" size="3000" seed="1" /></fields></region>
        <region name="crate"><boundary class="box"><size x="20000" y="10000" z="4000" /></boundary>
          <fields><volumetricfog medium="m" size="3000" seed="1" /></fields></region>
        <region name="everywhere">
          <fields><volumetricfog medium="m" size="100000" seed="1" /></fields></region>
      </regions>)",
      lib);
  REQUIRE(lib.regions.size() == 3);
  const RegionDef& disc = lib.regions.at("disc");
  const auto discSpheres = generateFogSpheres(disc, disc.fogs[0]);
  CHECK_FALSE(discSpheres.empty());
  for (const FogSphere& s : discSpheres) {
    CHECK(std::hypot(s.pos.x, s.pos.z) <= 25000.0);
    CHECK(std::abs(s.pos.y) <= 2500.0);
  }
  const RegionDef& crate = lib.regions.at("crate");
  for (const FogSphere& s : generateFogSpheres(crate, crate.fogs[0])) {
    CHECK(std::abs(s.pos.x) <= 10000.0);
    CHECK(std::abs(s.pos.y) <= 5000.0);
    CHECK(std::abs(s.pos.z) <= 2000.0);
  }
  const RegionDef& everywhere = lib.regions.at("everywhere");
  CHECK(everywhere.boundary == RegionBoundary::Unbounded);
  const auto all = generateFogSpheres(everywhere, everywhere.fogs[0]);
  CHECK_FALSE(all.empty());
  for (const FogSphere& s : all) CHECK(length(s.pos) <= kFogRange);
}

TEST_CASE("an all-zero falloff authors an empty region") {
  RegionLibrary lib;
  parseRegionDefinitions(
      R"(<regions><region name="off"><boundary class="sphere"><size r="50000" /></boundary>
           <falloff><radial><step position="0" value="0" /><step position="1" value="0" /></radial>
           </falloff><fields><volumetricfog medium="m" size="5000" seed="1" /></fields>
         </region></regions>)",
      lib);
  const RegionDef& off = lib.regions.at("off");
  CHECK(generateFogSpheres(off, off.fogs[0]).empty());
}

TEST_CASE("hostile region values are rejected at parse time") {
  RegionLibrary lib;
  parseRegionDefinitions(
      R"(<regions>
        <region name="tiny"><boundary class="sphere"><size r="1000" /></boundary>
          <fields><volumetricfog medium="m" size="1e-300" /></fields></region>
        <region name="far"><boundary class="splinetube"><size r="10" />
          <splineposition x="1e300" y="0" z="0" tx="1" ty="0" tz="0" inlength="0" outlength="1" />
          <splineposition x="0" y="0" z="0" tx="1" ty="0" tz="0" inlength="1" outlength="0" />
        </boundary></region>
        <region name="flat"><boundary class="splinetube"><size r="10" />
          <splineposition x="0" y="0" z="0" tx="0" ty="0" tz="0" inlength="0" outlength="1" />
          <splineposition x="9" y="0" z="0" tx="0" ty="0" tz="0" inlength="1" outlength="0" />
        </boundary></region>
        <region name="huge"><boundary class="sphere"><size r="1e300" /></boundary></region>
      </regions>)",
      lib);
  CHECK(lib.regions.at("tiny").fogs.empty());
  CHECK(lib.regions.count("far") == 0);   // one point left: no spline
  CHECK(lib.regions.count("flat") == 0);  // zero tangents
  CHECK(lib.regions.count("huge") == 0);
}

TEST_CASE("alias chains resolve up to eight hops") {
  RegionLibrary lib;
  lib.regions["end"] = RegionDef{};
  for (int i = 0; i < 9; ++i)
    lib.aliases["a" + std::to_string(i)] =
        i == 0 ? std::string("end") : "a" + std::to_string(i - 1);
  CHECK(lib.find("a6") != nullptr);
  CHECK(lib.find("a8") == nullptr);
}

TEST_CASE("parseClusterRegions drops a degenerate quaternion") {
  const auto placements = parseClusterRegions(
      R"(<macros><macro name="c_macro"><connections>
           <connection ref="regions"><offset><quaternion qx="0" qy="0" qz="0" qw="0" /></offset>
             <macro><properties><region ref="r" /></properties></macro></connection>
         </connections></macro></macros>)",
      "c_macro");
  CHECK(placements.empty());
}

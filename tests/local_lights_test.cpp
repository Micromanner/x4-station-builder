#include "x4sb/assetpipe/component.hpp"
#include "x4sb/data/catalog.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace x4sb;

namespace {
// Modelled on the real X4 schema (dock-area components), NOT extracted game assets:
// component-level layer lights plus a part light under a parented connection.
constexpr const char* kLightsComponent = R"(<?xml version="1.0"?>
<components>
  <component name="dock_test">
    <layers>
      <layer>
        <lights>
          <omni name="o1" r="169" g="214" b="255" range="37.7" intensity="1.5" specularintensity="1">
            <offset><position x="1" y="2" z="3"/></offset>
          </omni>
          <spot name="s1" angle="120" range="50" r="255" g="255" b="255" intensity="12">
            <offset><position x="0" y="10" z="0"/></offset>
          </spot>
          <box name="b1" r="200" g="200" b="200" intensity="1.2">
            <size x="77" y="28" z="28"/>
          </box>
          <arealight name="a1" areax="11.4" areay="0" radius="0.5" range="200" spotattenuation="0.72" intensity="400"/>
          <ambient name="amb" r="1" g="1" b="1" range="10" intensity="1"/>
          <omni name="bad" range="nan" intensity="1"/>
        </lights>
      </layer>
    </layers>
    <connections>
      <connection name="c_base" tags="part">
        <offset><position x="100" y="0" z="0"/></offset>
        <parts><part name="part_base"/></parts>
      </connection>
      <connection name="c_elevator" tags="part animation" parent="part_base">
        <offset><position x="0" y="50" z="0"/></offset>
        <parts>
          <part name="anim_elevator">
            <lights>
              <omni name="e1" range="10" intensity="2">
                <offset><position x="5" y="0" z="0"/></offset>
              </omni>
            </lights>
          </part>
        </parts>
      </connection>
    </connections>
  </component>
</components>)";
}  // namespace

TEST_CASE("parseComponentLights reads every light kind and drops the rest") {
  const std::vector<LocalLight> lights = parseComponentLights(kLightsComponent);
  REQUIRE(lights.size() == 5);  // <ambient> and the NaN light are dropped

  CHECK(lights[0].kind == LightKind::Omni);
  CHECK(lights[0].local.position.z == doctest::Approx(3));
  CHECK(lights[0].color.y == doctest::Approx(214));
  CHECK(lights[0].range == doctest::Approx(37.7));
  CHECK(lights[0].intensity == doctest::Approx(1.5));

  CHECK(lights[1].kind == LightKind::Spot);
  CHECK(lights[1].spotAngle == doctest::Approx(120));

  CHECK(lights[2].kind == LightKind::Box);
  CHECK(lights[2].boxSize.x == doctest::Approx(77));
  CHECK(lights[2].range == doctest::Approx(0));

  CHECK(lights[3].kind == LightKind::Area);
  CHECK(lights[3].areaX == doctest::Approx(11.4));
  CHECK(lights[3].areaRadius == doctest::Approx(0.5));
  CHECK(lights[3].spotAttenuation == doctest::Approx(0.72));
}

TEST_CASE("parseComponentLights carries part lights through the parent chain") {
  const std::vector<LocalLight> lights = parseComponentLights(kLightsComponent);
  REQUIRE(lights.size() == 5);
  const LocalLight& e = lights[4];
  CHECK(e.local.position.x == doctest::Approx(105));  // base 100 + light 5
  CHECK(e.local.position.y == doctest::Approx(50));
}

TEST_CASE("parseComponentLights tolerates malformed XML") {
  CHECK(parseComponentLights("<components><component>").empty());
  CHECK(parseComponentLights("").empty());
}

TEST_CASE("catalog JSON round-trips lights") {
  ModuleDef d;
  d.id = "lit";
  LocalLight l;
  l.kind = LightKind::Area;
  l.local.position = {1, 2, 3};
  l.local.rotation = {0, 0, 0, 1};
  l.color = {255, 216, 182};
  l.intensity = 400;
  l.specularIntensity = 0.5;
  l.range = 200;
  l.areaX = 11.4;
  l.areaRadius = 0.25;
  l.spotAttenuation = 0.72;
  d.lights.push_back(l);

  const ModuleCatalog cat = ModuleCatalog::loadFromJson(toCatalogJson({d}));
  const ModuleDef* r = cat.find("lit");
  REQUIRE(r != nullptr);
  REQUIRE(r->lights.size() == 1);
  const LocalLight& o = r->lights[0];
  CHECK(o.kind == LightKind::Area);
  CHECK(o.local.position.z == doctest::Approx(3));
  CHECK(o.local.rotation.z == doctest::Approx(1));
  CHECK(o.color.z == doctest::Approx(182));
  CHECK(o.intensity == doctest::Approx(400));
  CHECK(o.specularIntensity == doctest::Approx(0.5));
  CHECK(o.range == doctest::Approx(200));
  CHECK(o.areaX == doctest::Approx(11.4));
  CHECK(o.areaRadius == doctest::Approx(0.25));
  CHECK(o.spotAttenuation == doctest::Approx(0.72));
}

TEST_CASE("loadFromJson skips lights of unknown kind") {
  const ModuleCatalog cat =
      ModuleCatalog::loadFromJson(R"({"modules":[{"id":"m","lights":[{"kind":"laser"}]}]})");
  const ModuleDef* r = cat.find("m");
  REQUIRE(r != nullptr);
  CHECK(r->lights.empty());
}

#include "x4sb/assetpipe/lensfx.hpp"

#include "x4sb/assetpipe/xmf.hpp"  // XmfMesh for flareMeshFromXmf

#include <doctest/doctest.h>

#include <cmath>

using namespace x4sb::assetpipe;

namespace {

const char* kBaseXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<lenseffects>
  <lenseffect name="none">
    <parameters></parameters>
    <elements></elements>
  </lenseffect>
  <lenseffect name="sun_ol">
    <parameters scale="100" lengthscale="100" distancescale="linear"></parameters>
    <elements>
      <element name="halo" part="part_newglow_v1_yellow" position="1.0" minsize="300000000" maxsize="300000000" blendorder="behind" shinebehind="1" rotation="0" active="1" glow="1" facecamera="1"></element>
      <element name="rays" part="part_newrays_v1" position="1.0" minsize="40" maxsize="50" shinebehind="0" rotation="-2" active="1" glow="0" facecamera="1"></element>
      <element name="ghost" part="part_specials02" position="0.5" minsize="3" maxsize="3" shinebehind="0" rotation="0" active="1" glow="0"></element>
      <element name="inactive" part="part_newrays_v1" position="1.0" minsize="40" maxsize="50" shinebehind="0" rotation="0" active="0" glow="0"></element>
    </elements>
  </lenseffect>
</lenseffects>)";

const char* kDiffXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<diff>
  <add sel="/lenseffects" pos="prepend">
    <lenseffect name="sun_mercury">
      <parameters scale="100"></parameters>
      <elements>
        <element name="e1" part="part_newglow_v4" position="1.0" minsize="15" maxsize="15" shinebehind="0" rotation="0" active="1" glow="0" facecamera="1"></element>
      </elements>
    </lenseffect>
    <lenseffect name="sun_ol">
      <parameters scale="50"></parameters>
      <elements></elements>
    </lenseffect>
  </add>
</diff>)";

}  // namespace

TEST_CASE("parseLensEffects reads effects, elements, and parameters.scale") {
  const LensEffectMap fx = parseLensEffects(kBaseXml);
  REQUIRE(fx.count("sun_ol") == 1);
  CHECK(fx.count("none") == 1);
  const LensEffect& ol = fx.at("sun_ol");
  CHECK(ol.scale == doctest::Approx(100.0f));
  REQUIRE(ol.elements.size() == 3);  // active=0 dropped at parse
  CHECK(ol.elements[0].part == "part_newglow_v1_yellow");
  CHECK(ol.elements[0].position == doctest::Approx(1.0f));
  CHECK(ol.elements[0].minSize == doctest::Approx(3.0e8f));
  CHECK(ol.elements[0].glow);
  CHECK(ol.elements[0].shineBehind);
  CHECK(ol.elements[1].rotation == doctest::Approx(-2.0f));
  CHECK_FALSE(ol.elements[1].glow);
  CHECK(ol.elements[2].position == doctest::Approx(0.5f));
}

TEST_CASE("parseLensEffects survives hostile input") {
  CHECK(parseLensEffects("").empty());
  CHECK(parseLensEffects("not xml at all <<<").empty());
  CHECK(parseLensEffects("<lenseffects/>").empty());
  // NaN/inf/negative sizes: element dropped, parse keeps going.
  const LensEffectMap fx = parseLensEffects(R"(<lenseffects>
    <lenseffect name="bad">
      <parameters scale="0"></parameters>
      <elements>
        <element name="nan" part="p" position="1.0" minsize="nan" maxsize="1" active="1"></element>
        <element name="neg" part="p" position="1.0" minsize="-5" maxsize="1" active="1"></element>
        <element name="noname" position="1.0" minsize="1" maxsize="1" active="1"></element>
        <element name="ok" part="p" position="1.0" minsize="1" maxsize="2" active="1"></element>
      </elements>
    </lenseffect>
  </lenseffects>)");
  REQUIRE(fx.count("bad") == 1);
  CHECK(fx.at("bad").scale == doctest::Approx(100.0f));  // scale<=0 -> default
  REQUIRE(fx.at("bad").elements.size() == 1);
  CHECK(fx.at("bad").elements[0].minSize == doctest::Approx(1.0f));
}

TEST_CASE("mergeLensEffectsDiff adds new effects and overwrites collisions") {
  LensEffectMap fx = parseLensEffects(kBaseXml);
  mergeLensEffectsDiff(fx, kDiffXml);
  REQUIRE(fx.count("sun_mercury") == 1);
  CHECK(fx.at("sun_mercury").elements.size() == 1);
  // DLC wins on collision (spec: merge by name, DLC wins).
  CHECK(fx.at("sun_ol").scale == doctest::Approx(50.0f));
  CHECK(fx.at("sun_ol").elements.empty());
}

TEST_CASE("mergeLensEffectsDiff ignores non-diff and malformed docs") {
  LensEffectMap fx = parseLensEffects(kBaseXml);
  mergeLensEffectsDiff(fx, "garbage");
  mergeLensEffectsDiff(fx, kBaseXml);  // full doc, not a diff -> no-op
  CHECK(fx.size() == 2);
}

TEST_CASE("flareMeshFromXmf flattens to xy/uv/rgb and drops bad triangles") {
  x4sb::XmfMesh mesh;
  mesh.positions = {{-0.5, -0.5, 0.0}, {0.5, -0.5, -0.03}, {0.0, 0.5, 0.0}};
  mesh.texcoords = {{-0.5, 1.0}, {0.0, 1.5}};  // third vertex unpainted -> 0,0
  mesh.colors = {{200, 100, 50, 7}};           // others -> white
  mesh.indices = {0, 1, 2, 0, 1, 9};           // second triangle out of range
  const FlareMesh fm = flareMeshFromXmf(mesh);
  REQUIRE(fm.xy.size() == 6);
  CHECK(fm.xy[3] == doctest::Approx(-0.5f));
  CHECK(fm.uv[1] == doctest::Approx(1.0f));
  CHECK(fm.uv[2] == doctest::Approx(0.0f));
  CHECK(fm.uv[3] == doctest::Approx(1.5f));
  CHECK(fm.uv[4] == doctest::Approx(0.0f));
  CHECK(fm.rgb[0] == 200);
  CHECK(fm.rgb[5] == 255);
  CHECK(fm.index == std::vector<std::uint32_t>{0, 1, 2});

  CHECK(flareMeshFromXmf(x4sb::XmfMesh{}).index.empty());
  mesh.indices = {0, 1};  // not a triangle list
  CHECK(flareMeshFromXmf(mesh).index.empty());
  mesh.indices = {0, 1, 2};
  mesh.positions[0].x = std::nan("");
  CHECK(flareMeshFromXmf(mesh).index.empty());
}

TEST_CASE("makeSunSprite classifies world-metre vs screen-relative sizes") {
  LensElement el;
  el.part = "p";
  el.minSize = 3.0e8f;
  el.maxSize = 3.0e8f;
  el.glow = true;
  // Angular: 2*atan(maxSize/2 / distance), distance 3.64e8.
  const auto ang = makeSunSprite(el, 100.0f, 3.64e8, "lensflares/a.dds");
  REQUIRE(ang.has_value());
  CHECK_FALSE(ang->screenMode);
  CHECK(ang->size == doctest::Approx(2.0 * std::atan(1.5e8 / 3.64e8)));
  CHECK(ang->glow);
  CHECK(ang->texFile == "lensflares/a.dds");

  // Screen-relative: maxsize/scale (the captured game draws every screen
  // element at its maxsize).
  el.minSize = 40.0f;
  el.maxSize = 50.0f;
  el.glow = false;
  el.position = 0.5f;
  const auto scr = makeSunSprite(el, 100.0f, 3.64e8, "lensflares/a.dds");
  REQUIRE(scr.has_value());
  CHECK(scr->screenMode);
  CHECK(scr->size == doctest::Approx(0.5f));  // 50 / 100
  CHECK(scr->position == doctest::Approx(0.5f));

  // Angular size without a distance is underivable -> element skipped.
  el.minSize = 3.0e8f;
  el.maxSize = 3.0e8f;
  CHECK_FALSE(makeSunSprite(el, 100.0f, 0.0, "lensflares/a.dds").has_value());
}

TEST_CASE("makeSunSprite carries material diffuseStr as intensity") {
  LensElement el;
  el.part = "p";
  el.minSize = 40.0f;
  el.maxSize = 40.0f;
  const auto def = makeSunSprite(el, 100.0f, 0.0, "lensflares/a.dds");
  REQUIRE(def.has_value());
  CHECK(def->intensity == doctest::Approx(1.0f));  // omitted -> material default

  const auto str = makeSunSprite(el, 100.0f, 0.0, "lensflares/a.dds", 2.0f);
  REQUIRE(str.has_value());
  CHECK(str->intensity == doctest::Approx(2.0f));

  // Hostile values fall back to 1.0 (NaN, non-positive) or clamp (huge).
  const auto nan = makeSunSprite(el, 100.0f, 0.0, "lensflares/a.dds", std::nanf(""));
  REQUIRE(nan.has_value());
  CHECK(nan->intensity == doctest::Approx(1.0f));
  const auto neg = makeSunSprite(el, 100.0f, 0.0, "lensflares/a.dds", -3.0f);
  REQUIRE(neg.has_value());
  CHECK(neg->intensity == doctest::Approx(1.0f));
  const auto huge = makeSunSprite(el, 100.0f, 0.0, "lensflares/a.dds", 1.0e9f);
  REQUIRE(huge.has_value());
  CHECK(huge->intensity == doctest::Approx(100.0f));
}

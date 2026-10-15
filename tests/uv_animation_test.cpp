#include "x4sb/data/uv_animation.hpp"

#include "x4sb/assetpipe/component.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <string>

using namespace x4sb;

namespace {

UvAnimation oneChannel(std::vector<UvKey> v, double start, double end, int uvSet = 0) {
  UvAnimation a;
  a.loopStart = start;
  a.loopEnd = end;
  UvChannel c;
  c.uvSet = uvSet;
  c.v = std::move(v);
  a.channels.push_back(std::move(c));
  return a;
}

double vAt(const UvAnimation& a, double frame, int uvSet = 0) {
  return static_cast<double>(
      evalUvAnimation(a, frame / kAnimFps).offset[static_cast<std::size_t>(uvSet)].v);
}

// Synthetic fixture shaped like X4's anim_* parts (not a game asset, spec §12).
const char* kAnimComponent = R"(<?xml version="1.0"?>
<components>
  <component name="test_anim_01" class="connectionmodule">
    <source geometry="assets/test/test_anim_01_data"/>
    <connections>
      <connection name="Connection01" tags="part animation textureanimation">
        <animations><animation name="loop" start="1" end="100"/></animations>
        <parts>
          <part name="anim_scroll">
            <uv_animations>
              <uv_animation mapchannel="3">
                <uv_property name="u_offset" controller="linear_float">
                  <key frame="0" value="0"/><key frame="100" value="0"/>
                </uv_property>
                <uv_property name="v_offset" controller="linear_float">
                  <key frame="100" value="-4"/><key frame="0" value="0"/>
                </uv_property>
              </uv_animation>
              <uv_animation mapchannel="9">
                <uv_property name="u_offset" controller="linear_float">
                  <key frame="0" value="0"/><key frame="10" value="1"/>
                </uv_property>
              </uv_animation>
            </uv_animations>
          </part>
        </parts>
      </connection>
      <connection name="Connection02" tags="part animation">
        <parts>
          <part name="anim_blink">
            <uv_animations>
              <uv_animation mapchannel="1">
                <uv_property name="u_offset" controller="bezier_float">
                  <key frame="5" value="0" in_tangent_type="auto" out_tangent_type="step"/>
                  <key frame="20" value="-0.1" in_tangent_type="auto" out_tangent_type="auto"/>
                </uv_property>
              </uv_animation>
            </uv_animations>
          </part>
        </parts>
      </connection>
      <connection name="Connection03" tags="part">
        <parts><part name="part_main"/></parts>
      </connection>
      <connection name="Connection04" tags="part animation textureanimation">
        <animations><animation name="loop" start="0" end="6000"/></animations>
        <parts>
          <part name="part_corona">
            <uv_animations>
              <uv_animation mapchannel="2" rotaroundcenter="true">
                <uv_property name="rotation_angle" controller="bezier_float">
                  <key frame="0" value="0" in_tangent_type="auto" out_tangent_type="auto"/>
                  <key frame="6000" value="-360" in_tangent_type="auto" out_tangent_type="auto"/>
                </uv_property>
                <uv_property name="u_tile" controller="linear_float">
                  <key frame="0" value="2"/>
                </uv_property>
              </uv_animation>
            </uv_animations>
          </part>
        </parts>
      </connection>
      <connection name="Connection05" tags="part animation textureanimation cargodependent">
        <animations><animation name="scaling_range" start="0" end="100"/></animations>
        <parts>
          <part name="part_storage_fill">
            <uv_animations>
              <uv_animation mapchannel="1">
                <uv_property name="u_tile" controller="linear_float">
                  <key frame="0" value="1"/><key frame="100" value="50"/>
                </uv_property>
              </uv_animation>
            </uv_animations>
          </part>
        </parts>
      </connection>
    </connections>
  </component>
</components>)";

}  // namespace

TEST_CASE("uv animation: linear curve interpolates and clamps") {
  const UvAnimation a = oneChannel({{0, 0, UvInterp::Linear}, {100, -4, UvInterp::Linear}}, 0, 200);
  CHECK(vAt(a, 0) == doctest::Approx(0.0));
  CHECK(vAt(a, 25) == doctest::Approx(-1.0));
  CHECK(vAt(a, 150) == doctest::Approx(-4.0));  // past the last key
}

TEST_CASE("uv animation: the clock wraps over the loop range at 30 fps") {
  const UvAnimation a = oneChannel({{0, 0, UvInterp::Linear}, {100, -4, UvInterp::Linear}}, 1, 100);
  // 99-frame loop starting at frame 1: t = 0 and one full loop later land on frame 1.
  CHECK(vAt(a, 0) == doctest::Approx(-0.04));
  CHECK(vAt(a, 99) == doctest::Approx(-0.04));
  CHECK(vAt(a, 99 + 49) == doctest::Approx(-2.0));
  CHECK(static_cast<double>(evalUvAnimation(a, -5.0).offset[0].v) == doctest::Approx(-0.04));
}

TEST_CASE("uv animation: step holds and smooth keeps plateaus flat") {
  const UvAnimation step = oneChannel({{0, 0, UvInterp::Step}, {10, 1, UvInterp::Step}}, 0, 20);
  CHECK(vAt(step, 9.9) == doctest::Approx(0.0));
  CHECK(vAt(step, 10) == doctest::Approx(1.0));

  const UvAnimation blink = oneChannel({{0, 0, UvInterp::Smooth},
                                        {10, 0, UvInterp::Smooth},
                                        {15, -0.1, UvInterp::Smooth},
                                        {25, -0.1, UvInterp::Smooth},
                                        {30, 0, UvInterp::Smooth}},
                                       0, 30);
  CHECK(vAt(blink, 5) == doctest::Approx(0.0));
  CHECK(vAt(blink, 20) == doctest::Approx(-0.1));
  CHECK(vAt(blink, 12.5) == doctest::Approx(-0.05));  // symmetric ease between levels
  for (double f = 10; f <= 15; f += 0.5) {
    CHECK(vAt(blink, f) <= 1e-6);
    CHECK(vAt(blink, f) >= -0.1 - 1e-6);  // float offsets
  }

  const UvAnimation ramp = oneChannel({{0, 0, UvInterp::Smooth}, {10, 1, UvInterp::Smooth}}, 0, 20);
  // Two smooth keys ease in and out (flat end tangents).
  CHECK(vAt(ramp, 2.5) == doctest::Approx(0.15625));
  CHECK(vAt(ramp, 5) == doctest::Approx(0.5));
}

TEST_CASE("uv animation: empty and unanimated sets give the identity") {
  const UvTransforms none = evalUvAnimation(UvAnimation{}, 3.0);
  for (std::size_t s = 0; s < kUvSets; ++s) {
    CHECK(none.linear[s] == std::array<float, 4>{1, 0, 0, 1});
    CHECK(none.offset[s].u == 0.0F);
    CHECK(none.offset[s].v == 0.0F);
  }
  const UvAnimation a = oneChannel({{0, 0, UvInterp::Linear}, {10, 1, UvInterp::Linear}}, 0, 10, 2);
  const UvTransforms o = evalUvAnimation(a, 5.0 / kAnimFps);
  CHECK(o.offset[0].v == 0.0F);
  CHECK(static_cast<double>(o.offset[2].v) == doctest::Approx(0.5));
  CHECK(o.linear[2] == std::array<float, 4>{1, 0, 0, 1});  // offset-only: no turn, no tile
}

namespace {

// uv' = mat2(linear) * uv + offset, as the vertex shaders apply it.
std::array<double, 2> apply(const UvTransforms& t, std::size_t set, double u, double v) {
  const std::array<float, 4>& m = t.linear[set];
  return {static_cast<double>(m[0]) * u + static_cast<double>(m[2]) * v +
              static_cast<double>(t.offset[set].u),
          static_cast<double>(m[1]) * u + static_cast<double>(m[3]) * v +
              static_cast<double>(t.offset[set].v)};
}

}  // namespace

TEST_CASE("uv animation: rotation turns about the origin or the UV centre, tiling scales") {
  UvAnimation a;
  a.loopStart = 0;
  a.loopEnd = 400;
  UvChannel turn;
  turn.uvSet = 1;
  turn.rotation = {{0, 0, UvInterp::Linear}, {360, 360, UvInterp::Linear}};  // 1 degree per frame
  a.channels.push_back(turn);
  turn.uvSet = 2;
  turn.aroundCenter = true;
  a.channels.push_back(turn);
  UvChannel tile;
  tile.uvSet = 3;
  tile.uTile = {{0, 2, UvInterp::Linear}};
  tile.vTile = {{0, 4, UvInterp::Linear}};
  tile.u = {{0, 0.25, UvInterp::Linear}};
  a.channels.push_back(tile);

  const UvTransforms t = evalUvAnimation(a, 90.0 / kAnimFps);  // a quarter turn
  const std::array<double, 2> origin = apply(t, 1, 1, 0);
  CHECK(origin[0] == doctest::Approx(0.0).epsilon(1e-6));
  CHECK(origin[1] == doctest::Approx(1.0));
  const std::array<double, 2> pivot = apply(t, 2, 0.5, 0.5);  // the centre stays put
  CHECK(pivot[0] == doctest::Approx(0.5));
  CHECK(pivot[1] == doctest::Approx(0.5));
  const std::array<double, 2> edge = apply(t, 2, 1, 0.5);
  CHECK(edge[0] == doctest::Approx(0.5));
  CHECK(edge[1] == doctest::Approx(1.0));
  const std::array<double, 2> tiled =
      apply(t, 3, 0.5, 0.5);  // scaled about the origin, then offset
  CHECK(tiled[0] == doctest::Approx(1.25));
  CHECK(tiled[1] == doctest::Approx(2.0));
}

TEST_CASE("uv animation: json round-trip and hostile records") {
  UvAnimation a = oneChannel({{0, 0, UvInterp::Linear}, {10, 1, UvInterp::Smooth}}, 1, 10, 3);
  a.channels[0].u = {{0, 2, UvInterp::Step}};
  a.channels[0].rotation = {{0, 0, UvInterp::Linear}, {10, 90, UvInterp::Linear}};
  a.channels[0].vTile = {{0, 3, UvInterp::Step}};
  a.channels[0].aroundCenter = true;
  const UvAnimation back = uvAnimationFromJson(uvAnimationToJson(a));
  REQUIRE(back.channels.size() == 1);
  CHECK(back.loopStart == 1.0);
  CHECK(back.loopEnd == 10.0);
  CHECK(back.channels[0].uvSet == 3);
  REQUIRE(back.channels[0].v.size() == 2);
  CHECK(back.channels[0].v[1].interp == UvInterp::Smooth);
  CHECK(back.channels[0].u[0].interp == UvInterp::Step);
  CHECK(back.channels[0].rotation.size() == 2);
  CHECK(back.channels[0].uTile.empty());
  REQUIRE(back.channels[0].vTile.size() == 1);
  CHECK(back.channels[0].vTile[0].value == 3.0);
  CHECK(back.channels[0].aroundCenter);

  const nlohmann::json bad = nlohmann::json::parse(R"({"loop":[0,10],"channels":[
      {"set":4,"u":[[0,1,0]]},
      {"set":1,"u":[[0,1,7]],"v":[[5,0,0],[1,0,0]]},
      {"set":"x"},
      7]})");
  const UvAnimation h = uvAnimationFromJson(bad);
  REQUIRE(h.channels.size() == 1);  // set 4 and set "x" dropped
  CHECK(h.channels[0].u.empty());   // unknown interp
  CHECK(h.channels[0].v.empty());   // unsorted keys
  CHECK(uvAnimationFromJson(nlohmann::json::array()).channels.empty());

  // Finite but huge frames would overflow the curve maths to NaN.
  const UvAnimation huge = uvAnimationFromJson(
      nlohmann::json::parse(R"({"loop":[0,10],"channels":[{"set":0,"u":[[0,0,0],[1e300,1,0]]}]})"));
  REQUIRE(huge.channels.size() == 1);
  CHECK(huge.channels[0].u.empty());
}

TEST_CASE("component parse: uv_animations with loop range and key-span fallback") {
  const ComponentGeometry geo = parseComponentGeometry(kAnimComponent);
  REQUIRE(geo.parts.size() == 5);

  const UvAnimation& scroll = geo.parts[0].uvAnimation;
  CHECK(scroll.loopStart == 1.0);
  CHECK(scroll.loopEnd == 100.0);
  REQUIRE(scroll.channels.size() == 1);  // mapchannel 9 is out of range
  CHECK(scroll.channels[0].uvSet == 2);
  REQUIRE(scroll.channels[0].v.size() == 2);
  CHECK(scroll.channels[0].v[0].frame == 0.0);  // keys sorted
  CHECK(scroll.channels[0].v[1].value == -4.0);

  const UvAnimation& blink = geo.parts[1].uvAnimation;
  CHECK(blink.loopStart == 5.0);  // no loop animation: the keys' span
  CHECK(blink.loopEnd == 20.0);
  REQUIRE(blink.channels.size() == 1);
  CHECK(blink.channels[0].uvSet == 0);
  CHECK(blink.channels[0].u[0].interp == UvInterp::Step);
  CHECK(blink.channels[0].u[1].interp == UvInterp::Smooth);

  CHECK(geo.parts[2].uvAnimation.channels.empty());

  const UvAnimation& corona = geo.parts[3].uvAnimation;
  REQUIRE(corona.channels.size() == 1);  // a rotation-only set still animates
  CHECK(corona.channels[0].uvSet == 1);
  CHECK(corona.channels[0].aroundCenter);
  REQUIRE(corona.channels[0].rotation.size() == 2);
  CHECK(corona.channels[0].rotation[1].value == -360.0);
  REQUIRE(corona.channels[0].uTile.size() == 1);
  CHECK(corona.channels[0].vTile.empty());

  CHECK(geo.parts[4].uvAnimation.channels.empty());  // cargo-driven, not time
}

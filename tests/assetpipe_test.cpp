#include "x4sb/assetpipe/component.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

// Synthetic fixtures modelled on the real X4 schema (NOT extracted game assets —
// see spec §12). They mirror the structure of a station module's component &
// macro XML: a cube-shaped connection module with snap connectors on two faces,
// plus a non-snap structural part connection that must be filtered out.
namespace {

const char* kComponentXml = R"(<?xml version="1.0"?>
<components>
  <component name="test_cube_01" class="connectionmodule">
    <source geometry="assets/test/test_cube_01_data"/>
    <connections>
      <connection name="Connection01" tags="part  ">
        <offset/>
      </connection>
      <connection name="ConnectionSnap001" tags="snap ">
        <offset>
          <position x="0" y="0" z="200"/>
        </offset>
      </connection>
      <connection name="ConnectionSnap002" tags="snap ">
        <offset>
          <position x="0" y="0" z="-200"/>
          <quaternion qx="0" qy="-1" qz="0" qw="0"/>
        </offset>
      </connection>
    </connections>
  </component>
</components>)";

const char* kMacroXml = R"(<?xml version="1.0"?>
<macros>
  <macro name="test_cube_01_macro" class="connectionmodule">
    <component ref="test_cube_01"/>
    <properties>
      <identification name="{1,2}" makerrace="argon" description="{1,3}"/>
      <hull max="120000"/>
    </properties>
  </macro>
</macros>)";

}  // namespace

TEST_CASE("parseComponentConnections reads every connection with its offset") {
  const auto conns = parseComponentConnections(kComponentXml);
  REQUIRE(conns.size() == 3);

  CHECK(conns[0].name == "Connection01");
  CHECK(conns[0].tags == "part");  // normalized (trailing spaces gone)
  CHECK(conns[0].offset.position.z == doctest::Approx(0));

  CHECK(conns[1].name == "ConnectionSnap001");
  CHECK(conns[1].offset.position.z == doctest::Approx(200));
  // No <quaternion> -> identity rotation.
  CHECK(conns[1].offset.rotation.w == doctest::Approx(1));
}

TEST_CASE("snapConnectionPoints keeps only snap connectors and maps qx,qy,qz,qw") {
  const auto pts = snapConnectionPoints(kComponentXml);
  REQUIRE(pts.size() == 2);  // the two ConnectionSnap*, not Connection01

  CHECK(pts[0].id == "ConnectionSnap001");
  CHECK(pts[0].localPosition.z == doctest::Approx(200));
  CHECK(pts[0].type == "snap");

  CHECK(pts[1].id == "ConnectionSnap002");
  CHECK(pts[1].localPosition.z == doctest::Approx(-200));
  // X4 (qx,qy,qz,qw)=(0,-1,0,0) -> our Quat {w,x,y,z}={0,0,-1,0}: 180 deg about Y.
  CHECK(pts[1].localRotation.w == doctest::Approx(0));
  CHECK(pts[1].localRotation.y == doctest::Approx(-1));
}

TEST_CASE("parseMacro reads identity and maps class to Category") {
  const MacroInfo m = parseMacro(kMacroXml);
  CHECK(m.macroName == "test_cube_01_macro");
  CHECK(m.componentRef == "test_cube_01");
  CHECK(m.className == "connectionmodule");
  CHECK(m.makerRace == "argon");
  CHECK(m.category == Category::Connector);
}

TEST_CASE("categoryFromClass maps the X4 station-module classes") {
  CHECK(categoryFromClass("production") == Category::Production);
  CHECK(categoryFromClass("storage") == Category::Storage);
  CHECK(categoryFromClass("habitation") == Category::Habitat);
  CHECK(categoryFromClass("dockarea") == Category::Dock);
  CHECK(categoryFromClass("pier") == Category::Dock);
  CHECK(categoryFromClass("defence") == Category::Defense);
  CHECK(categoryFromClass("connectionmodule") == Category::Connector);
  CHECK(categoryFromClass("somethingelse") == Category::Other);
}

#include "x4sb/assetpipe/component.hpp"

#include <doctest/doctest.h>

#include <string>

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
        <offset>
          <position x="100" y="0" z="0"/>
        </offset>
        <parts>
          <part name="part_main">
            <size>
              <max x="10" y="20" z="30"/>
              <center x="1" y="2" z="3"/>
            </size>
          </part>
        </parts>
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

const char* kNoCollisionComponentXml = R"(<?xml version="1.0"?>
<components>
  <component name="test_nc_01" class="production">
    <connections>
      <connection name="Structural" tags="part  ">
        <offset><position x="0" y="0" z="0"/></offset>
        <parts><part name="part_main">
          <size><max x="100" y="50" z="100"/><center x="0" y="0" z="0"/></size>
        </part></parts>
      </connection>
      <connection name="Detail" tags="part detail_s nocollision  ">
        <offset/>
        <parts><part name="detail_big">
          <size><max x="9999" y="9999" z="9999"/><center x="0" y="0" z="0"/></size>
        </part></parts>
      </connection>
    </connections>
  </component>
</components>)";

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

TEST_CASE("moduleAabb unions part size boxes at their mount offsets") {
  // part-local box = center +/- max -> x[-9,11] y[-18,22] z[-27,33];
  // mounted at offset (100,0,0) -> x[91,111] y[-18,22] z[-27,33].
  const AABB box = moduleAabb(kComponentXml);
  CHECK(box.min.x == doctest::Approx(91));
  CHECK(box.max.x == doctest::Approx(111));
  CHECK(box.min.y == doctest::Approx(-18));
  CHECK(box.max.y == doctest::Approx(22));
  CHECK(box.min.z == doctest::Approx(-27));
  CHECK(box.max.z == doctest::Approx(33));
}

TEST_CASE("moduleAabb ignores nocollision parts") {
  // Only the collidable 'Structural' part counts; the huge nocollision 'Detail'
  // part (max 9999) must be excluded from the collision AABB.
  const AABB box = moduleAabb(kNoCollisionComponentXml);
  CHECK(box.min.x == doctest::Approx(-100));
  CHECK(box.max.x == doctest::Approx(100));
  CHECK(box.min.y == doctest::Approx(-50));
  CHECK(box.max.y == doctest::Approx(50));
  CHECK(box.min.z == doctest::Approx(-100));
  CHECK(box.max.z == doctest::Approx(100));
}

TEST_CASE("parseComponentGeometry reads the geometry folder and parts (good case unchanged)") {
  const ComponentGeometry geo = parseComponentGeometry(kComponentXml);
  // A normal single-slash path must pass through untouched.
  CHECK(geo.geometryFolder == "assets/test/test_cube_01_data");
  REQUIRE(geo.parts.size() == 1);
  CHECK(geo.parts[0].name == "part_main");
}

TEST_CASE("parseComponentGeometry normalizes a double-backslash / trailing-slash source path") {
  // Real X4 dock/landmark modules author geometry with a stray double-backslash
  // (e.g. struct "...\landmarks\\foo_data"); after the '\'->'/' pass that becomes
  // "...landmarks//foo_data", which no longer matches the single-slash archive
  // entry. parseComponentGeometry must collapse the run and strip a trailing '/'.
  const char* xml = R"(<?xml version="1.0"?>
<components>
  <component name="dockarea_xen_m_station_01" class="dockarea">
    <source geometry="assets\structures\landmarks\\dockarea_xen_m_station_01_data\"/>
    <connections>
      <connection name="C" tags="part"><parts><part name="part_main"/></parts></connection>
    </connections>
  </component>
</components>)";
  const ComponentGeometry geo = parseComponentGeometry(xml);
  CHECK(geo.geometryFolder == "assets/structures/landmarks/dockarea_xen_m_station_01_data");
  CHECK(geo.geometryFolder.find("//") == std::string::npos);  // no double-slash left
  CHECK(geo.geometryFolder.back() != '/');                    // no trailing slash
  REQUIRE(geo.parts.size() == 1);
  CHECK(geo.parts[0].name == "part_main");
}
